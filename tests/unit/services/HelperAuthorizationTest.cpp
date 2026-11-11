#include "helper/services/AsyncAuthorization.hpp"
#include "helper/services/WipePlanRegistry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace auth = async_authorization;
using namespace std::chrono_literals;

constexpr char INTERFACE[] = "test.StorageWiper.Authorization";
constexpr char PATH[] = "/test/Authorization";
constexpr char XML[] = R"(<node><interface name="test.StorageWiper.Authorization">
  <method name="Pending"><arg type="b" direction="out"/></method>
  <method name="Ready"><arg type="b" direction="out"/></method>
  <method name="StartPreparedWipe"><arg type="b" direction="out"/></method>
  <signal name="Progress"><arg type="u"/></signal>
</interface></node>)";

class HelperAuthorizationTest : public testing::Test {
protected:
    struct Reply {
        int completions = 0;
        bool success = false;
        GError* error = nullptr;
        ~Reply() { g_clear_error(&error); }
    };

    inline static GTestDBus* bus = nullptr;
    inline static HelperAuthorizationTest* current = nullptr;
    GDBusConnection* helper = nullptr;
    std::vector<GDBusConnection*> clients;
    std::vector<std::unique_ptr<Reply>> replies;
    std::vector<auth::AuthorizationPtr> decisions;
    std::map<std::string, int> dispatched;
    WipePlanRegistry plans;
    int wipe_starts = 0;
    std::chrono::milliseconds timeout = 3s;
    guint registration = 0;
    guint disconnect_subscription = 0;
    guint progress_subscription = 0;
    int progress_events = 0;

    static void SetUpTestSuite() {
        bus = g_test_dbus_new(G_TEST_DBUS_NONE);
        g_test_dbus_up(bus);
    }

    static void TearDownTestSuite() {
        g_test_dbus_down(bus);
        g_clear_object(&bus);
    }

    auto connect_bus() -> GDBusConnection* {
        GError* error = nullptr;
        auto* connection = g_dbus_connection_new_for_address_sync(
            g_test_dbus_get_bus_address(bus),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &error);
        EXPECT_EQ(error, nullptr) << (error ? error->message : "");
        g_clear_error(&error);
        return connection;
    }

    auto add_client() -> GDBusConnection* {
        auto* connection = connect_bus();
        clients.push_back(connection);
        return connection;
    }

    static auto pump_until(const std::function<bool()>& condition) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_iteration(nullptr, FALSE)) {}
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }

    static void dispatch(const gchar* method, GVariant*, GDBusMethodInvocation* invocation) {
        ++current->dispatched[method];
        if (std::string_view{method} == "StartPreparedWipe") {
            const auto plan = current->plans.take(
                {.sender = g_dbus_method_invocation_get_sender(invocation), .id = "pending-plan"});
            if (plan) {
                ++current->wipe_starts;
            }
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", plan.has_value()));
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
    }

    static void fake_authorizer(const auth::AuthorizationPtr& request) {
        current->decisions.push_back(request);
        if (request->method == "Ready") {
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                [](gpointer data) -> gboolean {
                    auth::verify_caller_and_dispatch(*static_cast<auth::AuthorizationPtr*>(data));
                    return G_SOURCE_REMOVE;
                },
                new auth::AuthorizationPtr(request),
                [](gpointer data) { delete static_cast<auth::AuthorizationPtr*>(data); });
        }
    }

    static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                            const gchar* method, GVariant* parameters,
                            GDBusMethodInvocation* invocation, gpointer) {
        auth::authorize_method(invocation, parameters, method, "test.wipe", dispatch,
                               fake_authorizer, current->timeout);
    }

    auto call(GDBusConnection* client, const char* method = "Pending") -> Reply& {
        auto& reply = *replies.emplace_back(std::make_unique<Reply>());
        g_dbus_connection_call(
            client, g_dbus_connection_get_unique_name(helper), PATH, INTERFACE, method,
            g_variant_new("()"), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 5'000, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                auto& state = *static_cast<Reply*>(data);
                auto* response =
                    g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &state.error);
                ++state.completions;
                if (response) {
                    gboolean success = FALSE;
                    g_variant_get(response, "(b)", &success);
                    state.success = success != FALSE;
                    g_variant_unref(response);
                }
            },
            &reply);
        return reply;
    }

    void SetUp() override {
        current = this;
        ASSERT_TRUE(auth::g_authorizations.empty());
        helper = connect_bus();
        ASSERT_NE(helper, nullptr);
        ASSERT_NE(add_client(), nullptr);
        GError* error = nullptr;
        auto* node = g_dbus_node_info_new_for_xml(XML, &error);
        ASSERT_EQ(error, nullptr);
        static const GDBusInterfaceVTable vtable{.method_call = method_call,
                                                 .get_property = nullptr,
                                                 .set_property = nullptr,
                                                 .padding = {nullptr}};
        registration = g_dbus_connection_register_object(helper, PATH, node->interfaces[0], &vtable,
                                                         nullptr, nullptr, &error);
        g_dbus_node_info_unref(node);
        ASSERT_EQ(error, nullptr);
        ASSERT_NE(registration, 0u);
        disconnect_subscription = g_dbus_connection_signal_subscribe(
            helper, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
            "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
               GVariant* parameters, gpointer) {
                const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
                g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
                if (name[0] == ':' && old_owner[0] != '\0' && new_owner[0] == '\0') {
                    auth::cancel_authorizations(name);
                }
            },
            nullptr, nullptr);
        progress_subscription = g_dbus_connection_signal_subscribe(
            clients.front(), g_dbus_connection_get_unique_name(helper), INTERFACE, "Progress", PATH,
            nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*,
               gpointer data) { ++static_cast<HelperAuthorizationTest*>(data)->progress_events; },
            this, nullptr);
    }

    void TearDown() override {
        auth::cancel_authorizations();
        for (auto* client : clients) {
            if (client && !g_dbus_connection_is_closed(client)) {
                g_dbus_connection_close_sync(client, nullptr, nullptr);
            }
        }
        EXPECT_TRUE(pump_until([&] {
            return std::all_of(replies.begin(), replies.end(),
                               [](const auto& reply) { return reply->completions == 1; });
        }));
        EXPECT_TRUE(auth::g_authorizations.empty());
        if (progress_subscription && !clients.empty()) {
            g_dbus_connection_signal_unsubscribe(clients.front(), progress_subscription);
        }
        if (disconnect_subscription) {
            g_dbus_connection_signal_unsubscribe(helper, disconnect_subscription);
        }
        if (registration) {
            g_dbus_connection_unregister_object(helper, registration);
        }
        decisions.clear();
        for (auto*& client : clients) {
            g_clear_object(&client);
        }
        if (helper) {
            g_dbus_connection_close_sync(helper, nullptr, nullptr);
        }
        g_clear_object(&helper);
        current = nullptr;
    }
};

TEST_F(HelperAuthorizationTest, PendingAuthenticationDoesNotBlockOtherRequestsOrProgress) {
    auto& pending = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    auto& ready = call(add_client(), "Ready");
    GError* error = nullptr;
    ASSERT_TRUE(g_dbus_connection_emit_signal(helper, nullptr, PATH, INTERFACE, "Progress",
                                              g_variant_new("(u)", 42u), &error));
    ASSERT_EQ(error, nullptr);
    ASSERT_TRUE(pump_until([&] { return ready.completions == 1 && progress_events == 1; }));
    EXPECT_TRUE(ready.success);
    EXPECT_EQ(pending.completions, 0);
    EXPECT_EQ(dispatched["Pending"], 0);
    auth::verify_caller_and_dispatch(decisions.front());
    ASSERT_TRUE(pump_until([&] { return pending.completions == 1; }));
    EXPECT_TRUE(pending.success);
    EXPECT_EQ(dispatched["Pending"], 1);
    auth::verify_caller_and_dispatch(decisions.front());
    EXPECT_EQ(dispatched["Pending"], 1);
}

TEST_F(HelperAuthorizationTest, TimeoutCancelsAndLateApprovalCannotDispatchOrReplyAgain) {
    timeout = 30ms;
    auto& reply = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    const auto request = decisions.front();
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    EXPECT_TRUE(g_error_matches(reply.error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT));
    EXPECT_TRUE(g_cancellable_is_cancelled(request->cancellable));
    auth::verify_caller_and_dispatch(request);
    auth::reject_authorization(request, G_DBUS_ERROR_ACCESS_DENIED, "late denial");
    EXPECT_EQ(reply.completions, 1);
    EXPECT_EQ(dispatched["Pending"], 0);
    EXPECT_TRUE(auth::g_authorizations.empty());
}

TEST_F(HelperAuthorizationTest, DeadlineRejectsApprovalBeforeTimeoutSourceRuns) {
    auto& reply = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    const auto request = decisions.front();
    request->deadline = auth::AuthorizationRequest::Clock::now() - 1ms;
    auth::verify_caller_and_dispatch(request);
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    EXPECT_TRUE(g_error_matches(reply.error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT));
    EXPECT_EQ(request->timeout_source, 0u);
    EXPECT_EQ(dispatched["Pending"], 0);
}

TEST_F(HelperAuthorizationTest, DeadlineIsCheckedAgainAfterAsynchronousCallerLookup) {
    auto& reply = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    const auto request = decisions.front();
    auth::verify_caller_and_dispatch(request);
    request->deadline = auth::AuthorizationRequest::Clock::now() - 1ms;
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    EXPECT_TRUE(g_error_matches(reply.error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT));
    EXPECT_EQ(dispatched["Pending"], 0);
}

TEST_F(HelperAuthorizationTest, DuplicateQueuedApprovalsDispatchAndReplyExactlyOnce) {
    auto& reply = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    auth::verify_caller_and_dispatch(decisions.front());
    auth::verify_caller_and_dispatch(decisions.front());
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    while (g_main_context_iteration(nullptr, FALSE)) {}
    EXPECT_TRUE(reply.success);
    EXPECT_EQ(dispatched["Pending"], 1);
    EXPECT_EQ(reply.completions, 1);
}

TEST_F(HelperAuthorizationTest, DiscardDuringPendingAuthorizationPreventsStartingWipe) {
    WipePlan plan;
    plan.id = "pending-plan";
    const std::string sender = g_dbus_connection_get_unique_name(clients.front());
    ASSERT_TRUE(plans.insert(sender, plan));
    auto& reply = call(clients.front(), "StartPreparedWipe");
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    ASSERT_TRUE(plans.discard({.sender = sender, .id = plan.id}));
    auth::verify_caller_and_dispatch(decisions.front());
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    EXPECT_FALSE(reply.success);
    EXPECT_EQ(wipe_starts, 0);
}

TEST_F(HelperAuthorizationTest, DisconnectCancelsPendingApproval) {
    auto& reply = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    const auto request = decisions.front();
    ASSERT_TRUE(g_dbus_connection_close_sync(clients.front(), nullptr, nullptr));
    ASSERT_TRUE(pump_until([&] { return auth::g_authorizations.empty(); }));
    EXPECT_TRUE(g_cancellable_is_cancelled(request->cancellable));
    auth::verify_caller_and_dispatch(request);
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    EXPECT_EQ(dispatched["Pending"], 0);
}

TEST_F(HelperAuthorizationTest, OwnerCheckRejectsDisconnectedCallerBeforeDisconnectNotification) {
    g_dbus_connection_signal_unsubscribe(helper, disconnect_subscription);
    disconnect_subscription = 0;
    auto& reply = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return decisions.size() == 1; }));
    const auto request = decisions.front();
    ASSERT_TRUE(g_dbus_connection_close_sync(clients.front(), nullptr, nullptr));
    auth::verify_caller_and_dispatch(request);
    ASSERT_TRUE(pump_until([&] { return auth::g_authorizations.empty(); }));
    ASSERT_TRUE(pump_until([&] { return reply.completions == 1; }));
    EXPECT_EQ(dispatched["Pending"], 0);
    EXPECT_TRUE(g_cancellable_is_cancelled(request->cancellable));
}

TEST_F(HelperAuthorizationTest, PerSenderLimitDoesNotBlockAnotherSender) {
    call(clients.front());
    call(clients.front());
    auto& excess = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return excess.completions == 1; }));
    EXPECT_TRUE(g_error_matches(excess.error, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED));
    EXPECT_EQ(decisions.size(), auth::MAX_AUTHORIZATIONS_PER_SENDER);
    auto& other = call(add_client(), "Ready");
    ASSERT_TRUE(pump_until([&] { return other.completions == 1; }));
    EXPECT_TRUE(other.success);
}

TEST_F(HelperAuthorizationTest, GlobalLimitBoundsPendingStateAndShutdownCancelsAll) {
    for (std::size_t i = 0; i < auth::MAX_AUTHORIZATIONS / auth::MAX_AUTHORIZATIONS_PER_SENDER;
         ++i) {
        auto* client = add_client();
        for (std::size_t j = 0; j < auth::MAX_AUTHORIZATIONS_PER_SENDER; ++j) {
            call(client);
        }
    }
    ASSERT_TRUE(pump_until([&] { return decisions.size() == auth::MAX_AUTHORIZATIONS; }));
    auto& excess = call(clients.front());
    ASSERT_TRUE(pump_until([&] { return excess.completions == 1; }));
    EXPECT_TRUE(g_error_matches(excess.error, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED));
    EXPECT_EQ(auth::g_authorizations.size(), auth::MAX_AUTHORIZATIONS);
    auth::cancel_authorizations();
    ASSERT_TRUE(pump_until([&] {
        return std::all_of(replies.begin(), replies.end(),
                           [](const auto& reply) { return reply->completions == 1; });
    }));
    for (const auto& request : decisions) {
        EXPECT_TRUE(g_cancellable_is_cancelled(request->cancellable));
        auth::verify_caller_and_dispatch(request);
    }
    EXPECT_EQ(dispatched["Pending"], 0);
}

}  // namespace
