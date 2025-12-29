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

#include "services/DiskService.hpp"
#include "services/WipeService.hpp"

#include <gio/gio.h>
#include <polkit/polkit.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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
std::unique_ptr<DiskService> g_disk_service;
std::unique_ptr<WipeService> g_wipe_service;
std::string g_current_wipe_device;
std::atomic<bool> g_wipe_in_progress{false};

// D-Bus introspection XML
const char* introspection_xml = R"XML(
<node>
  <interface name="su.kidoz.storage_wiper.Helper">
    <method name="GetDisks">
      <arg name="disks" type="a(sssxbbsbs)" direction="out"/>
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
    <method name="StartWipe">
      <arg name="device_path" type="s" direction="in"/>
      <arg name="algorithm_id" type="u" direction="in"/>
      <arg name="started" type="b" direction="out"/>
      <arg name="error_message" type="s" direction="out"/>
    </method>
    <method name="CancelWipe">
      <arg name="cancelled" type="b" direction="out"/>
    </method>
    <signal name="WipeProgress">
      <arg name="device_path" type="s"/>
      <arg name="percentage" type="d"/>
      <arg name="current_pass" type="i"/>
      <arg name="total_passes" type="i"/>
      <arg name="status" type="s"/>
      <arg name="is_complete" type="b"/>
      <arg name="has_error" type="b"/>
      <arg name="error_message" type="s"/>
    </signal>
  </interface>
</node>
)XML";

/**
 * Check polkit authorization for the calling process
 */
auto check_authorization(GDBusMethodInvocation* invocation,
                         const char* action_id) -> bool {
    GError* error = nullptr;

    // Get the sender's credentials
    const char* sender = g_dbus_method_invocation_get_sender(invocation);
    if (!sender) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_AUTH_FAILED, "Could not determine caller");
        return false;
    }

    // Create polkit authority
    PolkitAuthority* authority = polkit_authority_get_sync(nullptr, &error);
    if (!authority) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_AUTH_FAILED, "Could not get polkit authority: %s",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    // Create subject from the D-Bus sender
    PolkitSubject* subject = polkit_system_bus_name_new(sender);

    // Check authorization
    PolkitAuthorizationResult* result = polkit_authority_check_authorization_sync(
        authority,
        subject,
        action_id,
        nullptr,  // details
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
        nullptr,  // cancellable
        &error
    );

    g_object_unref(subject);
    g_object_unref(authority);

    if (!result) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_AUTH_FAILED, "Authorization check failed: %s",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    bool authorized = polkit_authorization_result_get_is_authorized(result);
    g_object_unref(result);

    if (!authorized) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_ACCESS_DENIED, "Not authorized for action: %s", action_id);
        return false;
    }

    return true;
}

/**
 * Emit WipeProgress signal on D-Bus
 */
void emit_wipe_progress(const WipeProgress& progress) {
    if (!g_connection) return;

    GError* error = nullptr;
    g_dbus_connection_emit_signal(
        g_connection,
        nullptr,  // broadcast to all
        DBUS_PATH,
        DBUS_INTERFACE,
        "WipeProgress",
        g_variant_new("(sdiiisbbs)",
            g_current_wipe_device.c_str(),
            progress.percentage,
            progress.current_pass,
            progress.total_passes,
            progress.status.c_str(),
            progress.is_complete ? TRUE : FALSE,
            progress.has_error ? TRUE : FALSE,
            progress.error_message.c_str()
        ),
        &error
    );

    if (error) {
        std::cerr << "Failed to emit signal: " << error->message << std::endl;
        g_error_free(error);
    }
}

/**
 * Handle GetDisks method call
 */
void handle_get_disks(GDBusMethodInvocation* invocation) {
    if (!check_authorization(invocation, POLKIT_ACTION_LIST_DISKS)) {
        return;
    }

    auto disks = g_disk_service->get_available_disks();

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(sssxbbsbs)"));

    for (const auto& disk : disks) {
        g_variant_builder_add(&builder, "(sssxbbsbs)",
            disk.path.c_str(),
            disk.model.c_str(),
            disk.serial.c_str(),
            static_cast<gint64>(disk.size_bytes),
            disk.is_removable ? TRUE : FALSE,
            disk.is_ssd ? TRUE : FALSE,
            disk.filesystem.c_str(),
            disk.is_mounted ? TRUE : FALSE,
            disk.mount_point.c_str()
        );
    }

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(a(sssxbbsbs))", &builder));
}

/**
 * Handle ValidateDevicePath method call
 */
void handle_validate_device_path(GDBusMethodInvocation* invocation,
                                  GVariant* parameters) {
    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    auto result = g_disk_service->validate_device_path(path ? path : "");

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(bs)",
            result.has_value() ? TRUE : FALSE,
            result.has_value() ? "" : result.error().message.c_str()
        )
    );
}

/**
 * Handle IsDeviceWritable method call
 */
void handle_is_device_writable(GDBusMethodInvocation* invocation,
                                GVariant* parameters) {
    if (!check_authorization(invocation, POLKIT_ACTION_LIST_DISKS)) {
        return;
    }

    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    bool writable = g_disk_service->is_disk_writable(path ? path : "");

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(b)", writable ? TRUE : FALSE));
}

/**
 * Handle UnmountDevice method call
 */
void handle_unmount_device(GDBusMethodInvocation* invocation,
                            GVariant* parameters) {
    // Use wipe-disk authorization since unmount is a precursor to wiping
    if (!check_authorization(invocation, POLKIT_ACTION_WIPE_DISK)) {
        return;
    }

    const char* path = nullptr;
    g_variant_get(parameters, "(&s)", &path);

    auto result = g_disk_service->unmount_disk(path ? path : "");

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(bs)",
            result.has_value() ? TRUE : FALSE,
            result.has_value() ? "" : result.error().message.c_str()
        )
    );
}

/**
 * Handle GetAlgorithms method call
 */
void handle_get_algorithms(GDBusMethodInvocation* invocation) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ussi)"));

    constexpr std::array algorithms = {
        WipeAlgorithm::ZERO_FILL,
        WipeAlgorithm::RANDOM_FILL,
        WipeAlgorithm::DOD_5220_22_M,
        WipeAlgorithm::SCHNEIER,
        WipeAlgorithm::VSITR,
        WipeAlgorithm::GOST_R_50739_95,
        WipeAlgorithm::GUTMANN
    };

    for (auto algo : algorithms) {
        g_variant_builder_add(&builder, "(ussi)",
            static_cast<guint32>(algo),
            g_wipe_service->get_algorithm_name(algo).c_str(),
            g_wipe_service->get_algorithm_description(algo).c_str(),
            g_wipe_service->get_pass_count(algo)
        );
    }

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(a(ussi))", &builder));
}

/**
 * Handle StartWipe method call
 */
void handle_start_wipe(GDBusMethodInvocation* invocation,
                       GVariant* parameters) {
    if (!check_authorization(invocation, POLKIT_ACTION_WIPE_DISK)) {
        return;
    }

    const char* device_path = nullptr;
    guint32 algorithm_id = 0;
    g_variant_get(parameters, "(&su)", &device_path, &algorithm_id);

    if (g_wipe_in_progress.load()) {
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(bs)", FALSE, "A wipe operation is already in progress"));
        return;
    }

    // Validate device path
    auto valid = g_disk_service->validate_device_path(device_path ? device_path : "");
    if (!valid) {
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(bs)", FALSE, valid.error().message.c_str()));
        return;
    }

    // Validate algorithm
    auto algorithm = static_cast<WipeAlgorithm>(algorithm_id);

    g_wipe_in_progress.store(true);
    g_current_wipe_device = device_path;

    // Start wipe in background thread
    std::thread wipe_thread([device_path = std::string(device_path), algorithm]() {
        auto progress_callback = [](const WipeProgress& progress) {
            // Schedule signal emission on main thread
            auto* progress_copy = new WipeProgress(progress);
            g_idle_add([](gpointer data) -> gboolean {
                auto* progress = static_cast<WipeProgress*>(data);
                emit_wipe_progress(*progress);
                delete progress;
                return G_SOURCE_REMOVE;
            }, progress_copy);
        };

        g_wipe_service->wipe_disk(device_path, algorithm, progress_callback);
        g_wipe_in_progress.store(false);
    });
    wipe_thread.detach();

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(bs)", TRUE, ""));
}

/**
 * Handle CancelWipe method call
 */
void handle_cancel_wipe(GDBusMethodInvocation* invocation) {
    if (!check_authorization(invocation, POLKIT_ACTION_WIPE_DISK)) {
        return;
    }

    bool cancelled = g_wipe_service->cancel_current_operation();

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(b)", cancelled ? TRUE : FALSE));
}

/**
 * D-Bus method call handler
 */
void handle_method_call(GDBusConnection* /*connection*/,
                        const gchar* /*sender*/,
                        const gchar* /*object_path*/,
                        const gchar* /*interface_name*/,
                        const gchar* method_name,
                        GVariant* parameters,
                        GDBusMethodInvocation* invocation,
                        gpointer /*user_data*/) {
    if (g_strcmp0(method_name, "GetDisks") == 0) {
        handle_get_disks(invocation);
    } else if (g_strcmp0(method_name, "ValidateDevicePath") == 0) {
        handle_validate_device_path(invocation, parameters);
    } else if (g_strcmp0(method_name, "IsDeviceWritable") == 0) {
        handle_is_device_writable(invocation, parameters);
    } else if (g_strcmp0(method_name, "UnmountDevice") == 0) {
        handle_unmount_device(invocation, parameters);
    } else if (g_strcmp0(method_name, "GetAlgorithms") == 0) {
        handle_get_algorithms(invocation);
    } else if (g_strcmp0(method_name, "StartWipe") == 0) {
        handle_start_wipe(invocation, parameters);
    } else if (g_strcmp0(method_name, "CancelWipe") == 0) {
        handle_cancel_wipe(invocation);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method: %s", method_name);
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
void on_name_acquired(GDBusConnection* connection,
                      const gchar* name,
                      gpointer /*user_data*/) {
    std::cout << "Acquired D-Bus name: " << name << std::endl;
    g_connection = connection;

    // Parse introspection data
    GError* error = nullptr;
    GDBusNodeInfo* introspection_data = g_dbus_node_info_new_for_xml(
        introspection_xml, &error);

    if (!introspection_data) {
        std::cerr << "Failed to parse introspection XML: "
                  << (error ? error->message : "unknown") << std::endl;
        g_clear_error(&error);
        g_main_loop_quit(g_main_loop);
        return;
    }

    // Register object
    guint registration_id = g_dbus_connection_register_object(
        connection,
        DBUS_PATH,
        introspection_data->interfaces[0],
        &interface_vtable,
        nullptr,  // user_data
        nullptr,  // user_data_free_func
        &error
    );

    g_dbus_node_info_unref(introspection_data);

    if (registration_id == 0) {
        std::cerr << "Failed to register object: "
                  << (error ? error->message : "unknown") << std::endl;
        g_clear_error(&error);
        g_main_loop_quit(g_main_loop);
        return;
    }

    std::cout << "D-Bus object registered at " << DBUS_PATH << std::endl;
}

/**
 * Callback when D-Bus name is lost
 */
void on_name_lost(GDBusConnection* /*connection*/,
                  const gchar* name,
                  gpointer /*user_data*/) {
    std::cerr << "Lost D-Bus name: " << name << std::endl;
    g_main_loop_quit(g_main_loop);
}

/**
 * Callback when bus is acquired
 */
void on_bus_acquired(GDBusConnection* /*connection*/,
                     const gchar* /*name*/,
                     gpointer /*user_data*/) {
    std::cout << "Bus acquired" << std::endl;
}

}  // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    // Check if running as root
    if (getuid() != 0) {
        std::cerr << "Error: This helper must run as root" << std::endl;
        return 1;
    }

    std::cout << "Storage Wiper Helper starting..." << std::endl;

    // Initialize services
    g_disk_service = std::make_unique<DiskService>();
    g_wipe_service = std::make_unique<WipeService>();

    // Create main loop
    g_main_loop = g_main_loop_new(nullptr, FALSE);

    // Request D-Bus name
    guint owner_id = g_bus_own_name(
        G_BUS_TYPE_SYSTEM,
        DBUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        nullptr,  // user_data
        nullptr   // user_data_free_func
    );

    // Run main loop
    g_main_loop_run(g_main_loop);

    // Cleanup
    g_bus_unown_name(owner_id);
    g_main_loop_unref(g_main_loop);
    g_wipe_service.reset();
    g_disk_service.reset();

    std::cout << "Storage Wiper Helper stopped" << std::endl;
    return 0;
}
