/**
 * @file main.cpp
 * @brief D-Bus system service helper for Storage Wiper
 *
 * This privileged helper runs as root and provides D-Bus methods for:
 * - Listing available disks
 * - Performing wipe operations
 * - Progress reporting via D-Bus signals
 *
 * Authorization is handled via polkit.
 */

#include "helper/services/AsyncAuthorization.hpp"
#include "helper/services/DiskService.hpp"
#include "helper/services/WipeJobRegistry.hpp"
#include "helper/services/WipePlanRegistry.hpp"
#include "helper/services/WipeService.hpp"
#include "services/DBusSignatures.hpp"
#include "services/DevicePolicy.hpp"
#include "services/WipeJobVariant.hpp"
#include "services/WipePlanVariant.hpp"
#include "util/Logger.hpp"

#include <gio/gio.h>
#include <glib-unix.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <polkit/polkit.h>

namespace {

// D-Bus interface name and object path
constexpr auto DBUS_NAME = "su.kidoz.storage_wiper.Helper";
constexpr auto DBUS_PATH = "/su/kidoz/storage_wiper/Helper";
constexpr auto DBUS_INTERFACE = "su.kidoz.storage_wiper.Helper";

// Polkit action IDs
constexpr auto POLKIT_ACTION_LIST_DISKS = "su.kidoz.storage_wiper.list-disks";
constexpr auto POLKIT_ACTION_WIPE_DISK = "su.kidoz.storage_wiper.wipe-disk";

// Global state
GDBusConnection* g_connection = nullptr;
GMainLoop* g_main_loop = nullptr;
std::shared_ptr<DiskService> g_disk_service;
std::unique_ptr<WipeService> g_wipe_service;
WipePlanRegistry g_wipe_plans;
WipeJobRegistry g_wipe_jobs;
bool g_stopping = false;
guint g_shutdown_poll = 0;

// D-Bus introspection XML
// GetDisks return type: dbus_signatures::DISK_ARRAY (see DBusSignatures.hpp)
//   s=path, s=model, s=serial, x=size_bytes, b=is_removable, b=is_ssd,
//   s=filesystem, b=is_mounted, s=mount_point, u=smart_status
//   (0=unknown,1=good,2=warning,3=critical),
//   b=smart_available, b=smart_healthy, x=power_on_hours,
//   i=reallocated_sectors, i=pending_sectors, i=temperature_celsius,
//   i=uncorrectable_errors, i=percentage_used, i=available_spare_percent,
//   i=available_spare_threshold_percent (-1 means unknown),
//   b=is_partition, s=parent_disk (empty for whole disks)
namespace {

/// Join NUL-terminated literals at compile time (no static initialiser can throw)
template <std::size_t... N>
consteval auto concat_literals(const char (&... parts)[N]) {
    std::array<char, (0 + ... + (N - 1)) + 1> out{};
    std::size_t pos = 0;
    ((std::copy_n(parts, N - 1, out.begin() + static_cast<std::ptrdiff_t>(pos)), pos += N - 1),
     ...);
    out[pos] = '\0';
    return out;
}

// The GetDisks array type is spliced in from DBusSignatures.hpp so the advertised
// interface can never drift from the format strings the code actually uses.
constexpr char INTROSPECTION_HEAD[] = R"XML(
<node>
  <interface name="su.kidoz.storage_wiper.Helper">
    <method name="GetDisks">
      <arg name="disks" type=")XML";
constexpr char INTROSPECTION_MIDDLE[] = R"XML(" direction="out"/>
    </method>
    <method name="GetDisks2">
      <arg name="version" type="u" direction="out"/>
      <arg name="disks" type=")XML";
constexpr char INTROSPECTION_TAIL[] = R"XML(" direction="out"/>
    </method>
    <method name="GetDiskSMART">
      <arg name="path" type="s" direction="in"/>
      <arg name="available" type="b" direction="out"/>
      <arg name="healthy" type="b" direction="out"/>
      <arg name="power_on_hours" type="x" direction="out"/>
      <arg name="reallocated_sectors" type="i" direction="out"/>
      <arg name="pending_sectors" type="i" direction="out"/>
      <arg name="temperature_celsius" type="i" direction="out"/>
      <arg name="uncorrectable_errors" type="i" direction="out"/>
      <arg name="percentage_used" type="i" direction="out"/>
      <arg name="available_spare_percent" type="i" direction="out"/>
      <arg name="available_spare_threshold_percent" type="i" direction="out"/>
      <arg name="status" type="u" direction="out"/>
    </method>
    <method name="ValidateDevicePath">
      <arg name="path" type="s" direction="in"/>
      <arg name="valid" type="b" direction="out"/>
      <arg name="error_message" type="s" direction="out"/>
    </method>
    <method name="IsDeviceWritable">
      <arg name="path" type="s" direction="in"/>
      <arg name="writable" type="b" direction="out"/>
    </method>
    <method name="UnmountDevice">
      <arg name="path" type="s" direction="in"/>
      <arg name="success" type="b" direction="out"/>
      <arg name="error_message" type="s" direction="out"/>
    </method>
    <method name="GetAlgorithms">
      <arg name="algorithms" type="a(ussi)" direction="out"/>
    </method>
    <method name="PrepareWipe">
      <arg name="device_path" type="s" direction="in"/>
      <arg name="algorithm_id" type="u" direction="in"/>
      <arg name="verify" type="b" direction="in"/>
      <arg name="plan" type="a{sv}" direction="out"/>
    </method>
    <method name="StartPreparedWipe">
      <arg name="plan_id" type="s" direction="in"/>
      <arg name="started" type="b" direction="out"/>
      <arg name="error_message" type="s" direction="out"/>
    </method>
    <method name="DiscardWipePlan">
      <arg name="plan_id" type="s" direction="in"/>
      <arg name="discarded" type="b" direction="out"/>
    </method>
    <method name="GetWipeJobs">
      <arg name="jobs" type="aa{sv}" direction="out"/>
    </method>
    <method name="CancelPreparedWipe">
      <arg name="plan_id" type="s" direction="in"/>
      <arg name="cancelled" type="b" direction="out"/>
    </method>
    <method name="StartWipe">
      <arg name="device_path" type="s" direction="in"/>
      <arg name="algorithm_id" type="u" direction="in"/>
      <arg name="verify" type="b" direction="in"/>
      <arg name="started" type="b" direction="out"/>
      <arg name="error_message" type="s" direction="out"/>
    </method>
    <method name="CancelWipe">
      <arg name="device_path" type="s" direction="in"/>
      <arg name="cancelled" type="b" direction="out"/>
    </method>
    <signal name="PreparedWipeProgress">
      <arg name="plan_id" type="s"/>
      <arg name="progress" type="(sdiisbbstttxbbbdt)"/>
    </signal>
    <signal name="WipeProgress">
      <arg name="device_path" type="s"/>
      <arg name="percentage" type="d"/>
      <arg name="current_pass" type="i"/>
      <arg name="total_passes" type="i"/>
      <arg name="status" type="s"/>
      <arg name="is_complete" type="b"/>
      <arg name="has_error" type="b"/>
      <arg name="error_message" type="s"/>
      <arg name="bytes_written" type="t"/>
      <arg name="total_bytes" type="t"/>
      <arg name="speed_bytes_per_sec" type="t"/>
      <arg name="estimated_seconds_remaining" type="x"/>
      <arg name="verification_enabled" type="b"/>
      <arg name="verification_in_progress" type="b"/>
      <arg name="verification_passed" type="b"/>
      <arg name="verification_percentage" type="d"/>
      <arg name="bad_block_count" type="t"/>
    </signal>
  </interface>
</node>
)XML";

}  // namespace

constexpr auto introspection_xml =
    concat_literals(INTROSPECTION_HEAD, dbus_signatures::DISK_ARRAY, INTROSPECTION_MIDDLE,
                    dbus_signatures::DISK_ARRAY_V2, INTROSPECTION_TAIL);

auto is_supported_algorithm(WipeAlgorithm algorithm) -> bool {
    constexpr std::array supported_algorithms = {
        WipeAlgorithm::ZERO_FILL, WipeAlgorithm::RANDOM_FILL,     WipeAlgorithm::DOD_5220_22_M,
        WipeAlgorithm::SCHNEIER,  WipeAlgorithm::VSITR,           WipeAlgorithm::GOST_R_50739_95,
        WipeAlgorithm::GUTMANN,   WipeAlgorithm::ATA_SECURE_ERASE};

    return std::find(supported_algorithms.begin(), supported_algorithms.end(), algorithm) !=
           supported_algorithms.end();
}

using async_authorization::authorize_method;
using async_authorization::cancel_authorizations;

/**
 * Emit WipeProgress signal on D-Bus
 */
void emit_wipe_progress(const std::string& plan_id, const std::string& device_path,
                        const WipeProgress& progress) {
    if (!g_wipe_jobs.update(plan_id, progress)) {
        return;
    }
    if (!g_connection)
        return;

    GError* error = nullptr;
    g_dbus_connection_emit_signal(
        g_connection,
        nullptr,  // broadcast to all
        DBUS_PATH, DBUS_INTERFACE, "PreparedWipeProgress",
        g_variant_new(
            "(s@(sdiisbbstttxbbbdt))", plan_id.c_str(),
            g_variant_new(
                dbus_signatures::WIPE_PROGRESS, device_path.c_str(), progress.percentage,
                progress.current_pass, progress.total_passes, progress.status.c_str(),
                progress.is_complete ? TRUE : FALSE, progress.has_error ? TRUE : FALSE,
                progress.error_message.c_str(), static_cast<guint64>(progress.bytes_written),
                static_cast<guint64>(progress.total_bytes),
                static_cast<guint64>(progress.speed_bytes_per_sec),
                static_cast<gint64>(progress.estimated_seconds_remaining),
                progress.verification_enabled ? TRUE : FALSE,
                progress.verification_in_progress ? TRUE : FALSE,
                progress.verification_passed ? TRUE : FALSE, progress.verification_percentage,
                static_cast<guint64>(progress.bad_block_count))),
        &error);

    if (error) {
        LOG_ERROR("Helper", std::format("Failed to emit signal: {}", error->message));
        g_error_free(error);
    }
}

/**
 * Handle GetDisks method call
 */
void handle_get_disks(GDBusMethodInvocation* invocation, bool version2 = false) {
    auto disks = g_disk_service->get_available_disks_sync();

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE(version2 ? dbus_signatures::DISK_ARRAY_V2
                                                             : dbus_signatures::DISK_ARRAY));

    for (const auto& disk : disks) {
        // Convert SmartData::HealthStatus to uint32
        auto smart_status = static_cast<guint32>(disk.smart.status);
        g_variant_builder_add(
            &builder, version2 ? dbus_signatures::DISK_RECORD_V2 : dbus_signatures::DISK_RECORD,
            disk.path.c_str(), disk.model.c_str(), disk.serial.c_str(),
            static_cast<gint64>(disk.size_bytes), disk.is_removable ? TRUE : FALSE,
            disk.is_ssd ? TRUE : FALSE, disk.filesystem.c_str(), disk.is_mounted ? TRUE : FALSE,
            disk.mount_point.c_str(), smart_status, disk.smart.available ? TRUE : FALSE,
            disk.smart.healthy ? TRUE : FALSE, static_cast<gint64>(disk.smart.power_on_hours),
            disk.smart.reallocated_sectors, disk.smart.pending_sectors,
            disk.smart.temperature_celsius, disk.smart.uncorrectable_errors,
            disk.smart.percentage_used, disk.smart.available_spare_percent,
            disk.smart.available_spare_threshold_percent, disk.is_partition ? TRUE : FALSE,
            disk.parent_disk.c_str(), disk.is_lvm_pv ? TRUE : FALSE,
            disk.has_dm_holders ? TRUE : FALSE);
    }

    g_dbus_method_invocation_return_value(
        invocation, version2 ? g_variant_new(dbus_signatures::DISK_LIST_REPLY_V2,
                                             dbus_signatures::DISK_CONTRACT_VERSION, &builder)
                             : g_variant_new(dbus_signatures::DISK_LIST_REPLY, &builder));
}

/**
 * Handle GetDiskSMART method call
 */
void handle_get_disk_smart(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    auto smart = g_disk_service->get_smart_data(path ? path : "");

    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new(dbus_signatures::SMART_RECORD, smart.available ? TRUE : FALSE,
                      smart.healthy ? TRUE : FALSE, static_cast<gint64>(smart.power_on_hours),
                      smart.reallocated_sectors, smart.pending_sectors, smart.temperature_celsius,
                      smart.uncorrectable_errors, smart.percentage_used,
                      smart.available_spare_percent, smart.available_spare_threshold_percent,
                      static_cast<guint32>(smart.status)));
}

/**
 * Handle ValidateDevicePath method call
 */
void handle_validate_device_path(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    auto result = g_disk_service->validate_device_path(path ? path : "");

    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(bs)", result.has_value() ? TRUE : FALSE,
                                  result.has_value() ? "" : result.error().message.c_str()));
}

/**
 * Handle IsDeviceWritable method call
 */
void handle_is_device_writable(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    bool writable = g_disk_service->is_disk_writable(path ? path : "");

    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(b)", writable ? TRUE : FALSE));
}

/**
 * Handle UnmountDevice method call
 */
void handle_unmount_device(GDBusMethodInvocation* invocation, GVariant* parameters) {
    // Use wipe-disk authorization since unmount is a precursor to wiping

    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    auto result = g_disk_service->unmount_disk(path ? path : "");

    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(bs)", result.has_value() ? TRUE : FALSE,
                                  result.has_value() ? "" : result.error().message.c_str()));
}

/**
 * Handle GetAlgorithms method call
 */
void handle_get_algorithms(GDBusMethodInvocation* invocation) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ussi)"));

    constexpr std::array algorithms = {
        WipeAlgorithm::ZERO_FILL, WipeAlgorithm::RANDOM_FILL,     WipeAlgorithm::DOD_5220_22_M,
        WipeAlgorithm::SCHNEIER,  WipeAlgorithm::VSITR,           WipeAlgorithm::GOST_R_50739_95,
        WipeAlgorithm::GUTMANN,   WipeAlgorithm::ATA_SECURE_ERASE};

    for (auto algo : algorithms) {
        g_variant_builder_add(&builder, "(ussi)", static_cast<guint32>(algo),
                              g_wipe_service->get_algorithm_name(algo).c_str(),
                              g_wipe_service->get_algorithm_description(algo).c_str(),
                              g_wipe_service->get_pass_count(algo));
    }

    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ussi))", &builder));
}

/**
 * Handle StartWipe method call
 */
void handle_prepare_wipe(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* path = nullptr;
    guint32 algorithm_id = 0;
    gboolean verify = FALSE;
    g_variant_get(parameters, "(&sub)", &path, &algorithm_id, &verify);
    const auto algorithm = static_cast<WipeAlgorithm>(algorithm_id);
    if (!is_supported_algorithm(algorithm)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "Unsupported wipe algorithm");
        return;
    }
    auto plan = g_wipe_service->prepare_plan(path, algorithm, verify != FALSE);
    if (!plan) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "%s",
                                              plan.error().message.c_str());
        return;
    }
    auto* id = g_uuid_string_random();
    plan->id = id;
    g_free(id);
    auto stored = g_wipe_plans.insert(g_dbus_method_invocation_get_sender(invocation), *plan);
    if (!stored) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_LIMITS_EXCEEDED, "%s",
                                              stored.error().message.c_str());
        return;
    }
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(@a{sv})", wipe_plan_variant::encode(*plan)));
}

void handle_discard_plan(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* id = nullptr;
    g_variant_get(parameters, "(&s)", &id);
    const auto discarded =
        g_wipe_plans.discard({.sender = g_dbus_method_invocation_get_sender(invocation), .id = id});
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", discarded));
}

void handle_start_wipe(GDBusMethodInvocation* invocation, GVariant*) {
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(bs)", FALSE,
                                  "Path-only StartWipe is disabled. Update the client and prepare "
                                  "and confirm a wipe plan."));
}

void handle_start_prepared_wipe(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* id = nullptr;
    g_variant_get(parameters, "(&s)", &id);
    auto plan =
        g_wipe_plans.take({.sender = g_dbus_method_invocation_get_sender(invocation), .id = id});
    if (!plan) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(bs)", FALSE, plan.error().message.c_str()));
        return;
    }
    if (auto registered = g_wipe_jobs.begin(*plan); !registered) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(bs)", FALSE, registered.error().message.c_str()));
        return;
    }
    const auto device = plan->device.disk.path;
    // The device path travels with every progress callback so concurrent
    // wipes are distinguishable on the bus.
    auto progress_callback = [device, plan_id = plan->id](const WipeProgress& progress) {
        // Schedule signal emission on main thread
        struct Payload {
            std::string id;
            std::string device;
            WipeProgress progress;
        };
        auto* payload = new Payload{plan_id, device, progress};
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* event = static_cast<Payload*>(data);
                emit_wipe_progress(event->id, event->device, event->progress);
                delete event;
                return G_SOURCE_REMOVE;
            },
            payload);
    };

    const auto started = g_wipe_service->start_prepared(*plan, progress_callback);
    if (!started) {
        g_wipe_jobs.abandon(plan->id);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new(dbus_signatures::START_WIPE_REPLY, FALSE,
                                      started.error().message.c_str()));
        return;
    }

    g_dbus_method_invocation_return_value(
        invocation, g_variant_new(dbus_signatures::START_WIPE_REPLY, TRUE, ""));
}

/**
 * Handle CancelWipe method call
 */
void handle_cancel_wipe(GDBusMethodInvocation* invocation, GVariant*) {
    // A reusable path cannot identify the job the caller meant to cancel.
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
}

void handle_get_wipe_jobs(GDBusMethodInvocation* invocation) {
    GVariantBuilder jobs;
    g_variant_builder_init(&jobs, G_VARIANT_TYPE("aa{sv}"));
    for (const auto& job : g_wipe_jobs.snapshots()) {
        g_variant_builder_add_value(&jobs, wipe_job_variant::encode(job));
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(aa{sv})", &jobs));
}

void handle_cancel_prepared_wipe(GDBusMethodInvocation* invocation, GVariant* parameters) {
    const char* id = nullptr;
    g_variant_get(parameters, "(&s)", &id);
    const auto target = g_wipe_jobs.cancellation_target(id);
    const bool cancelled = target && g_wipe_service->cancel_operation(*target);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", cancelled));
}

void begin_shutdown() {
    if (g_stopping) {
        return;
    }
    g_stopping = true;
    cancel_authorizations();
    for (const auto& id : g_wipe_jobs.cancellable_jobs()) {
        if (const auto path = g_wipe_jobs.cancellation_target(id)) {
            g_wipe_service->cancel_operation(*path);
        }
    }
    LOG_INFO("Helper", "Stopping: waiting for active wipes and firmware monitoring to finish");
    if (g_wipe_jobs.active_count() == 0) {
        g_main_loop_quit(g_main_loop);
        return;
    }
    g_shutdown_poll = g_timeout_add(
        100,
        [](gpointer) -> gboolean {
            if (g_wipe_jobs.active_count() != 0) {
                return G_SOURCE_CONTINUE;
            }
            g_shutdown_poll = 0;
            g_main_loop_quit(g_main_loop);
            return G_SOURCE_REMOVE;
        },
        nullptr);
}

/**
 * D-Bus method call handler
 */
void dispatch_authorized(const gchar* method_name, GVariant* parameters,
                         GDBusMethodInvocation* invocation) {
    if (g_stopping && (g_strcmp0(method_name, "PrepareWipe") == 0 ||
                       g_strcmp0(method_name, "StartPreparedWipe") == 0 ||
                       g_strcmp0(method_name, "UnmountDevice") == 0)) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_NO_SERVER,
            "The helper is stopping; no new destructive work is accepted");
        return;
    }
    if (g_strcmp0(method_name, "GetDisks") == 0) {
        handle_get_disks(invocation);
    } else if (g_strcmp0(method_name, "GetDisks2") == 0) {
        handle_get_disks(invocation, true);
    } else if (g_strcmp0(method_name, "GetDiskSMART") == 0) {
        handle_get_disk_smart(invocation, parameters);
    } else if (g_strcmp0(method_name, "ValidateDevicePath") == 0) {
        handle_validate_device_path(invocation, parameters);
    } else if (g_strcmp0(method_name, "IsDeviceWritable") == 0) {
        handle_is_device_writable(invocation, parameters);
    } else if (g_strcmp0(method_name, "UnmountDevice") == 0) {
        handle_unmount_device(invocation, parameters);
    } else if (g_strcmp0(method_name, "GetAlgorithms") == 0) {
        handle_get_algorithms(invocation);
    } else if (g_strcmp0(method_name, "PrepareWipe") == 0) {
        handle_prepare_wipe(invocation, parameters);
    } else if (g_strcmp0(method_name, "StartPreparedWipe") == 0) {
        handle_start_prepared_wipe(invocation, parameters);
    } else if (g_strcmp0(method_name, "DiscardWipePlan") == 0) {
        handle_discard_plan(invocation, parameters);
    } else if (g_strcmp0(method_name, "StartWipe") == 0) {
        handle_start_wipe(invocation, parameters);
    } else if (g_strcmp0(method_name, "GetWipeJobs") == 0) {
        handle_get_wipe_jobs(invocation);
    } else if (g_strcmp0(method_name, "CancelPreparedWipe") == 0) {
        handle_cancel_prepared_wipe(invocation, parameters);
    } else if (g_strcmp0(method_name, "CancelWipe") == 0) {
        handle_cancel_wipe(invocation, parameters);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method: %s", method_name);
    }
}

void handle_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                        const gchar* method_name, GVariant* parameters,
                        GDBusMethodInvocation* invocation, gpointer) {
    const std::string_view method{method_name};
    if (g_stopping &&
        (method == "PrepareWipe" || method == "StartPreparedWipe" || method == "UnmountDevice")) {
        dispatch_authorized(method_name, parameters, invocation);
        return;
    }
    if (method == "StartWipe" || method == "DiscardWipePlan") {
        dispatch_authorized(method_name, parameters, invocation);
    } else if (method == "PrepareWipe" || method == "StartPreparedWipe" || method == "CancelWipe" ||
               method == "UnmountDevice" || method == "CancelPreparedWipe") {
        authorize_method(invocation, parameters, method_name, POLKIT_ACTION_WIPE_DISK,
                         dispatch_authorized);
    } else if (method == "GetDisks" || method == "GetDiskSMART" || method == "ValidateDevicePath" ||
               method == "IsDeviceWritable" || method == "GetAlgorithms" || method == "GetDisks2" ||
               method == "GetWipeJobs") {
        authorize_method(invocation, parameters, method_name, POLKIT_ACTION_LIST_DISKS,
                         dispatch_authorized);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method: %s", method_name);
    }
}

// D-Bus interface vtable
const GDBusInterfaceVTable interface_vtable = {
    .method_call = handle_method_call,
    .get_property = nullptr,
    .set_property = nullptr,
    .padding = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}
};

/**
 * Callback when D-Bus name is acquired
 */
void on_name_acquired(GDBusConnection* connection, const gchar* name, gpointer /*user_data*/) {
    LOG_INFO("Helper", std::format("Acquired D-Bus name: {}", name));
    g_connection = connection;

    // Parse introspection data
    GError* error = nullptr;
    GDBusNodeInfo* introspection_data =
        g_dbus_node_info_new_for_xml(introspection_xml.data(), &error);

    if (!introspection_data) {
        LOG_ERROR("Helper", std::format("Failed to parse introspection XML: {}",
                                        error ? error->message : "unknown"));
        g_clear_error(&error);
        g_main_loop_quit(g_main_loop);
        return;
    }

    // Register object
    guint registration_id = g_dbus_connection_register_object(
        connection, DBUS_PATH, introspection_data->interfaces[0], &interface_vtable,
        nullptr,  // user_data
        nullptr,  // user_data_free_func
        &error);

    g_dbus_node_info_unref(introspection_data);

    if (registration_id == 0) {
        LOG_ERROR("Helper",
                  std::format("Failed to register object: {}", error ? error->message : "unknown"));
        g_clear_error(&error);
        g_main_loop_quit(g_main_loop);
        return;
    }

    g_dbus_connection_signal_subscribe(
        connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
           GVariant* parameters, gpointer) {
            const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
            g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
            if (name[0] == ':' && old_owner[0] != '\0' && new_owner[0] == '\0') {
                g_wipe_plans.remove_sender(name);
                cancel_authorizations(name);
            }
        },
        nullptr, nullptr);
    LOG_INFO("Helper", std::format("D-Bus object registered at {}", DBUS_PATH));
}

/**
 * Callback when D-Bus name is lost
 */
void on_name_lost(GDBusConnection* /*connection*/, const gchar* name, gpointer /*user_data*/) {
    LOG_ERROR("Helper", std::format("Lost D-Bus name: {}", name));
    g_connection = nullptr;
    begin_shutdown();
}

/**
 * Callback when bus is acquired
 */
void on_bus_acquired(GDBusConnection* /*connection*/, const gchar* /*name*/,
                     gpointer /*user_data*/) {
    LOG_INFO("Helper", "Bus acquired");
}

}  // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    // Initialize logger for helper daemon
    if (!util::Logger::instance().initialize("/var/log/storage-wiper", "storage-wiper-helper")) {
        util::Logger::instance().set_console_output(true);
    }

    // Check if running as root
    if (getuid() != 0) {
        LOG_ERROR("Helper", "This helper must run as root");
        return 1;
    }

    LOG_INFO("Helper", "Storage Wiper Helper starting...");

    // Initialize services
    g_disk_service = std::make_shared<DiskService>();
    g_wipe_service = std::make_unique<WipeService>(g_disk_service);

    // Create main loop
    g_main_loop = g_main_loop_new(nullptr, FALSE);

    // Request D-Bus name
    guint owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM, DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                    on_bus_acquired, on_name_acquired, on_name_lost,
                                    nullptr,  // user_data
                                    nullptr   // user_data_free_func
    );

    const auto signal_handler = [](gpointer) -> gboolean {
        begin_shutdown();
        return G_SOURCE_CONTINUE;
    };
    const guint term_source = g_unix_signal_add(SIGTERM, signal_handler, nullptr);
    const guint int_source = g_unix_signal_add(SIGINT, signal_handler, nullptr);

    // Run main loop
    g_main_loop_run(g_main_loop);

    // Cleanup
    g_source_remove(term_source);
    g_source_remove(int_source);
    if (g_shutdown_poll != 0) {
        g_source_remove(g_shutdown_poll);
        g_shutdown_poll = 0;
    }
    cancel_authorizations();
    g_bus_unown_name(owner_id);
    g_main_loop_unref(g_main_loop);
    g_wipe_service.reset();
    g_disk_service.reset();

    LOG_INFO("Helper", "Storage Wiper Helper stopped");
    return 0;
}
