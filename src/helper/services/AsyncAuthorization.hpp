#pragma once

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <polkit/polkit.h>

namespace async_authorization {

// Requests and callbacks are confined to the helper's main context. Keeping
// invocation/parameters alive lets the main loop continue serving cancellation
// and progress while the authentication agent is waiting for a person.
using AuthorizedHandler = void (*)(const gchar*, GVariant*, GDBusMethodInvocation*);

struct AuthorizationRequest {
    using Clock = std::chrono::steady_clock;
    guint64 id = 0;
    AuthorizedHandler handler = nullptr;
    std::string sender;
    std::string method;
    std::string action;
    GDBusMethodInvocation* invocation = nullptr;
    GVariant* parameters = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    guint timeout_source = 0;
    Clock::time_point deadline;

    ~AuthorizationRequest() {
        if (timeout_source != 0) {
            g_source_remove(timeout_source);
        }
        g_clear_object(&cancellable);
        g_clear_object(&invocation);
        if (parameters) {
            g_variant_unref(parameters);
        }
    }
};

using AuthorizationPtr = std::shared_ptr<AuthorizationRequest>;
inline std::map<guint64, AuthorizationPtr> g_authorizations;
inline guint64 g_next_authorization = 0;
constexpr auto AUTHORIZATION_TIMEOUT = std::chrono::seconds{20};
constexpr std::size_t MAX_AUTHORIZATIONS = 16;
constexpr std::size_t MAX_AUTHORIZATIONS_PER_SENDER = 2;

inline void remove_authorization(const AuthorizationPtr& request) {
    g_authorizations.erase(request->id);
    if (request->timeout_source != 0) {
        const auto source = request->timeout_source;
        request->timeout_source = 0;
        g_source_remove(source);
    }
}

inline void reject_authorization(const AuthorizationPtr& request, GDBusError code,
                                 const char* message) {
    if (!g_authorizations.contains(request->id)) {
        return;
    }
    remove_authorization(request);
    g_cancellable_cancel(request->cancellable);
    g_dbus_method_invocation_return_error(request->invocation, G_DBUS_ERROR, code, "%s", message);
}

inline auto authorization_is_pending(const AuthorizationPtr& request) -> bool {
    if (!g_authorizations.contains(request->id)) {
        return false;
    }
    // A busy main loop may deliver the polkit completion before the timeout
    // source. Check the actual deadline before every continuation regardless.
    if (AuthorizationRequest::Clock::now() >= request->deadline) {
        reject_authorization(request, G_DBUS_ERROR_TIMED_OUT,
                             "Authorization timed out; retry the request");
        return false;
    }
    return true;
}

inline void cancel_authorizations(const std::string& sender = {}) {
    std::vector<AuthorizationPtr> cancelled;
    for (const auto& [id, request] : g_authorizations) {
        (void)id;
        if (sender.empty() || request->sender == sender) {
            cancelled.push_back(request);
        }
    }
    for (const auto& request : cancelled) {
        reject_authorization(request, G_DBUS_ERROR_DISCONNECTED, "Caller or helper disconnected");
    }
}

inline void on_authorized_caller_checked(GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<AuthorizationPtr> holder(static_cast<AuthorizationPtr*>(data));
    const auto request = *holder;
    GError* error = nullptr;
    // Keep GLib's runtime type check at the asynchronous callback boundary.
    // NOLINTNEXTLINE(bugprone-casting-through-void)
    GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    gboolean owner_present = FALSE;
    if (reply) {
        g_variant_get(reply, "(b)", &owner_present);
        g_variant_unref(reply);
    }
    if (authorization_is_pending(request)) {
        if (!owner_present) {
            reject_authorization(request, G_DBUS_ERROR_DISCONNECTED,
                                 "The calling D-Bus connection is no longer available");
        } else {
            remove_authorization(request);
            request->handler(request->method.c_str(), request->parameters, request->invocation);
        }
    }
    g_clear_error(&error);
}

inline void verify_caller_and_dispatch(const AuthorizationPtr& request) {
    if (!authorization_is_pending(request)) {
        return;
    }
    // A disconnect notification can race the polkit completion. Ask the
    // bus whether the original unique sender still exists before dispatch.
    g_dbus_connection_call(g_dbus_method_invocation_get_connection(request->invocation),
                           "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "NameHasOwner", g_variant_new("(s)", request->sender.c_str()),
                           G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 5'000,
                           request->cancellable, on_authorized_caller_checked,
                           new AuthorizationPtr(request));
}

inline void on_authorization_checked(GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<AuthorizationPtr> holder(static_cast<AuthorizationPtr*>(data));
    const auto request = *holder;
    GError* error = nullptr;
    // Keep GLib's runtime type check at the asynchronous callback boundary.
    // NOLINTNEXTLINE(bugprone-casting-through-void)
    auto* authority = POLKIT_AUTHORITY(source);
    auto* decision = polkit_authority_check_authorization_finish(authority, result, &error);
    if (authorization_is_pending(request)) {
        if (!decision || !polkit_authorization_result_get_is_authorized(decision)) {
            reject_authorization(request, G_DBUS_ERROR_ACCESS_DENIED,
                                 error ? error->message : "Not authorized for this action");
        } else {
            verify_caller_and_dispatch(request);
        }
    }
    g_clear_object(&decision);
    g_clear_error(&error);
}

inline void on_authority_ready(GObject*, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<AuthorizationPtr> holder(static_cast<AuthorizationPtr*>(data));
    const auto request = *holder;
    GError* error = nullptr;
    auto* authority = polkit_authority_get_finish(result, &error);
    if (authorization_is_pending(request)) {
        if (!authority) {
            reject_authorization(request, G_DBUS_ERROR_AUTH_FAILED,
                                 error ? error->message : "Could not get polkit authority");
        } else {
            auto* subject = polkit_system_bus_name_new(request->sender.c_str());
            polkit_authority_check_authorization(
                authority, subject, request->action.c_str(), nullptr,
                POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, request->cancellable,
                on_authorization_checked, new AuthorizationPtr(request));
            g_object_unref(subject);
        }
    }
    g_clear_object(&authority);
    g_clear_error(&error);
}

using AuthorizationStarter = void (*)(const AuthorizationPtr&);

inline void start_polkit_authorization(const AuthorizationPtr& request) {
    polkit_authority_get_async(request->cancellable, on_authority_ready,
                               new AuthorizationPtr(request));
}

inline void authorize_method(GDBusMethodInvocation* invocation, GVariant* parameters,
                             const char* method, const char* action, AuthorizedHandler handler,
                             AuthorizationStarter starter = start_polkit_authorization,
                             std::chrono::milliseconds timeout = AUTHORIZATION_TIMEOUT) {
    const char* sender = g_dbus_method_invocation_get_sender(invocation);
    if (!sender || sender[0] != ':') {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED,
                                              "Could not determine unique caller");
        return;
    }
    const auto count =
        std::count_if(g_authorizations.begin(), g_authorizations.end(),
                      [sender](const auto& item) { return item.second->sender == sender; });
    if (g_authorizations.size() >= MAX_AUTHORIZATIONS ||
        count >= static_cast<std::ptrdiff_t>(MAX_AUTHORIZATIONS_PER_SENDER)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_LIMITS_EXCEEDED,
                                              "Too many pending authorization requests");
        return;
    }
    auto request = std::make_shared<AuthorizationRequest>();
    request->id = ++g_next_authorization;
    request->handler = handler;
    request->sender = sender;
    request->method = method;
    request->action = action;
    request->invocation = invocation;
    g_object_ref(request->invocation);
    request->parameters = g_variant_ref(parameters);
    request->deadline = AuthorizationRequest::Clock::now() + timeout;
    g_authorizations.emplace(request->id, request);
    request->timeout_source = g_timeout_add_full(
        G_PRIORITY_DEFAULT, static_cast<guint>(timeout.count()),
        [](gpointer data) -> gboolean {
            const auto pending = *static_cast<AuthorizationPtr*>(data);
            pending->timeout_source = 0;
            reject_authorization(pending, G_DBUS_ERROR_TIMED_OUT,
                                 "Authorization timed out; retry the request");
            return G_SOURCE_REMOVE;
        },
        new AuthorizationPtr(request),
        [](gpointer data) { delete static_cast<AuthorizationPtr*>(data); });
    starter(request);
}

}  // namespace async_authorization
