/**
 * @file DBusClient.cpp
 * @brief Implementation of D-Bus client for storage-wiper-helper
 */

#include "services/DBusClient.hpp"

#include "util/Logger.hpp"

#include <format>

namespace {
constexpr auto DBUS_NAME = "su.kidoz.storage_wiper.Helper";
constexpr auto DBUS_PATH = "/su/kidoz/storage_wiper/Helper";
constexpr auto DBUS_INTERFACE = "su.kidoz.storage_wiper.Helper";
constexpr auto DBUS_TIMEOUT_MS = 30'000;  // 30 second timeout for polkit dialogs
}  // namespace

DBusClient::DBusClient() = default;

DBusClient::~DBusClient() {
    cleanup();
}

auto DBusClient::get_connection_state() const -> ConnectionState {
    std::lock_guard lock(state_mutex_);
    return connection_state_;
}

void DBusClient::set_connection_state_callback(ConnectionStateCallback callback) {
    std::lock_guard lock(state_callback_mutex_);
    state_callback_ = std::move(callback);
}

auto DBusClient::request_reconnect() -> bool {
    std::lock_guard lock(state_mutex_);

    // Only allow reconnect if disconnected or failed
    if (connection_state_ == ConnectionState::CONNECTED ||
        connection_state_ == ConnectionState::CONNECTING ||
        connection_state_ == ConnectionState::RECONNECTING) {
        return false;
    }

    // Reset reconnect state and try again
    reconnect_attempts_ = 0;
    schedule_reconnect();
    return true;
}

auto DBusClient::is_service_available() const -> bool {
    if (!connection_)
        return false;

    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(
        connection_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", DBUS_NAME), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    if (!result) {
        g_clear_error(&error);
        return false;
    }

    gboolean has_owner = FALSE;
    g_variant_get(result, "(b)", &has_owner);
    g_variant_unref(result);

    return has_owner != FALSE;
}

void DBusClient::set_state(ConnectionState new_state, const std::string& error_message) {
    ConnectionState old_state;
    {
        std::lock_guard lock(state_mutex_);
        old_state = connection_state_;
        connection_state_ = new_state;
    }

    if (old_state != new_state) {
        notify_state_change(new_state, error_message);
    }
}

void DBusClient::notify_state_change(ConnectionState state, const std::string& message) {
    ConnectionStateCallback callback;
    {
        std::lock_guard lock(state_callback_mutex_);
        callback = state_callback_;
    }

    if (callback) {
        callback(state, message);
    }
}

void DBusClient::schedule_reconnect() {
    // Cancel any existing timer
    if (reconnect_timer_id_ != 0) {
        g_source_remove(reconnect_timer_id_);
        reconnect_timer_id_ = 0;
    }

    if (reconnect_attempts_ >= MAX_RECONNECT_ATTEMPTS) {
        set_state(ConnectionState::FAILED, "Maximum reconnection attempts exceeded");
        return;
    }

    set_state(ConnectionState::RECONNECTING, "");

    int delay = get_retry_delay_ms();
    reconnect_timer_id_ = g_timeout_add(delay, on_reconnect_timer, this);
}

auto DBusClient::attempt_reconnect() -> bool {
    reconnect_attempts_++;

    // Clean up existing resources but keep name watcher
    if (signal_subscription_id_ != 0 && connection_) {
        g_dbus_connection_signal_unsubscribe(connection_, signal_subscription_id_);
        signal_subscription_id_ = 0;
    }

    // Safely clean up old proxy with mutex protection
    {
        std::lock_guard lock(proxy_mutex_);
        if (proxy_) {
            g_object_unref(proxy_);
            proxy_ = nullptr;
        }
    }

    // Don't close the connection - we need it for name watching
    // Just try to create a new proxy
    GError* error = nullptr;

    if (!connection_) {
        connection_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!connection_) {
            std::string msg = error ? error->message : "unknown error";
            g_clear_error(&error);
            schedule_reconnect();
            return false;
        }
        // Restart name watching with new connection
        start_name_watching();
    }

    GDBusProxy* new_proxy =
        g_dbus_proxy_new_sync(connection_, G_DBUS_PROXY_FLAGS_NONE, nullptr, DBUS_NAME, DBUS_PATH,
                              DBUS_INTERFACE, nullptr, &error);

    if (!new_proxy) {
        std::string msg = error ? error->message : "unknown error";
        g_clear_error(&error);
        schedule_reconnect();
        return false;
    }

    // Safely assign new proxy with mutex protection
    {
        std::lock_guard lock(proxy_mutex_);
        proxy_ = new_proxy;
    }

    // Successfully reconnected
    setup_signal_handler();
    reset_reconnect_state();
    set_state(ConnectionState::CONNECTED, "");

    // Reload algorithms since we have a fresh connection
    algorithms_loaded_ = false;

    return true;
}

void DBusClient::reset_reconnect_state() {
    reconnect_attempts_ = 0;
    if (reconnect_timer_id_ != 0) {
        g_source_remove(reconnect_timer_id_);
        reconnect_timer_id_ = 0;
    }
}

auto DBusClient::get_retry_delay_ms() -> int {
    // Exponential backoff: 500ms, 1s, 2s, 4s, 8s... up to 30s
    int base_delay = INITIAL_RETRY_DELAY_MS * (1 << reconnect_attempts_);
    base_delay = std::min(base_delay, MAX_RETRY_DELAY_MS);

    // Add jitter (±25% of delay)
    std::uniform_int_distribution<int> dist(-base_delay / 4, base_delay / 4);
    return base_delay + dist(rng_);
}

void DBusClient::start_name_watching() {
    if (name_watcher_id_ != 0)
        return;

    name_watcher_id_ = g_bus_watch_name(G_BUS_TYPE_SYSTEM, DBUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                        on_name_appeared, on_name_vanished, this,
                                        nullptr  // user_data_free_func
    );
}

void DBusClient::stop_name_watching() {
    if (name_watcher_id_ != 0) {
        g_bus_unwatch_name(name_watcher_id_);
        name_watcher_id_ = 0;
    }
}

gboolean DBusClient::on_reconnect_timer(gpointer user_data) {
    auto* self = static_cast<DBusClient*>(user_data);
    self->reconnect_timer_id_ = 0;  // Timer is one-shot
    self->attempt_reconnect();
    return G_SOURCE_REMOVE;
}

void DBusClient::on_name_appeared(GDBusConnection* /*connection*/, const gchar* /*name*/,
                                  const gchar* /*name_owner*/, gpointer user_data) {
    auto* self = static_cast<DBusClient*>(user_data);

    ConnectionState current_state;
    {
        std::lock_guard lock(self->state_mutex_);
        current_state = self->connection_state_;
    }

    // If we're not connected, try to connect now
    if (current_state != ConnectionState::CONNECTED) {
        // Cancel any pending reconnect timer - service is available now
        if (self->reconnect_timer_id_ != 0) {
            g_source_remove(self->reconnect_timer_id_);
            self->reconnect_timer_id_ = 0;
        }
        self->reconnect_attempts_ = 0;
        self->attempt_reconnect();
    }
}

void DBusClient::on_name_vanished(GDBusConnection* /*connection*/, const gchar* /*name*/,
                                  gpointer user_data) {
    auto* self = static_cast<DBusClient*>(user_data);

    ConnectionState current_state;
    {
        std::lock_guard lock(self->state_mutex_);
        current_state = self->connection_state_;
    }

    // Only trigger reconnection if we were connected
    if (current_state == ConnectionState::CONNECTED) {
        // Safely clear the proxy with mutex protection
        {
            std::lock_guard lock(self->proxy_mutex_);
            if (self->proxy_) {
                g_object_unref(self->proxy_);
                self->proxy_ = nullptr;
            }
        }

        self->set_state(ConnectionState::DISCONNECTED, "Helper service stopped");
        self->schedule_reconnect();
    }
}

void DBusClient::cleanup() {
    // Stop reconnection attempts
    reset_reconnect_state();

    // Stop name watching
    stop_name_watching();

    if (signal_subscription_id_ != 0 && connection_) {
        g_dbus_connection_signal_unsubscribe(connection_, signal_subscription_id_);
        signal_subscription_id_ = 0;
    }

    {
        std::lock_guard lock(proxy_mutex_);
        if (proxy_) {
            g_object_unref(proxy_);
            proxy_ = nullptr;
        }
    }

    if (connection_) {
        g_object_unref(connection_);
        connection_ = nullptr;
    }

    set_state(ConnectionState::DISCONNECTED, "");
}

auto DBusClient::connect() -> bool {
    set_state(ConnectionState::CONNECTING, "");

    GError* error = nullptr;

    // Connect to system bus
    connection_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!connection_) {
        std::string msg = error ? error->message : "unknown";
        LOG_ERROR("DBusClient", std::format("Failed to connect to system bus: {}", msg));
        g_clear_error(&error);
        set_state(ConnectionState::DISCONNECTED, msg);
        return false;
    }

    // Start watching for the helper service name
    start_name_watching();

    // Create proxy for the helper service
    proxy_ = g_dbus_proxy_new_sync(connection_, G_DBUS_PROXY_FLAGS_NONE,
                                   nullptr,  // interface info
                                   DBUS_NAME, DBUS_PATH, DBUS_INTERFACE,
                                   nullptr,  // cancellable
                                   &error);

    if (!proxy_) {
        std::string msg = error ? error->message : "unknown";
        LOG_ERROR("DBusClient", std::format("Failed to create D-Bus proxy: {}", msg));
        g_clear_error(&error);
        // Don't fully cleanup - keep connection and name watcher for reconnection
        set_state(ConnectionState::DISCONNECTED, msg);
        // Schedule automatic reconnection attempt
        schedule_reconnect();
        return false;
    }

    // Set up signal handler for WipeProgress
    setup_signal_handler();

    set_state(ConnectionState::CONNECTED, "");
    return true;
}

auto DBusClient::is_connected() const -> bool {
    std::lock_guard lock(proxy_mutex_);
    return proxy_ != nullptr;
}

void DBusClient::setup_signal_handler() {
    if (!connection_)
        return;

    signal_subscription_id_ = g_dbus_connection_signal_subscribe(
        connection_, DBUS_NAME, DBUS_INTERFACE, "WipeProgress", DBUS_PATH,
        nullptr,  // arg0
        G_DBUS_SIGNAL_FLAGS_NONE, on_signal_received, this,
        nullptr   // user_data_free_func
    );
}

void DBusClient::on_signal_received(GDBusConnection* /*connection*/, const gchar* /*sender_name*/,
                                    const gchar* /*object_path*/, const gchar* /*interface_name*/,
                                    const gchar* signal_name, GVariant* parameters,
                                    gpointer user_data) {
    auto* self = static_cast<DBusClient*>(user_data);

    if (g_strcmp0(signal_name, "WipeProgress") != 0)
        return;

    // Parse progress signal
    const gchar* device_path = nullptr;
    gdouble percentage = 0.0;
    gint current_pass = 0;
    gint total_passes = 0;
    const gchar* status = nullptr;
    gboolean is_complete = FALSE;
    gboolean has_error = FALSE;
    const gchar* error_message = nullptr;
    guint64 bytes_written = 0;
    guint64 total_bytes = 0;
    guint64 speed_bytes_per_sec = 0;
    gint64 estimated_seconds_remaining = -1;
    gboolean verification_enabled = FALSE;
    gboolean verification_in_progress = FALSE;
    gboolean verification_passed = FALSE;
    gdouble verification_percentage = 0.0;

    g_variant_get(parameters, "(&sdii&sbb&stttxbbbd)", &device_path, &percentage, &current_pass,
                  &total_passes, &status, &is_complete, &has_error, &error_message, &bytes_written,
                  &total_bytes, &speed_bytes_per_sec, &estimated_seconds_remaining,
                  &verification_enabled, &verification_in_progress, &verification_passed,
                  &verification_percentage);

    WipeProgress progress{.bytes_written = bytes_written,
                          .total_bytes = total_bytes,
                          .current_pass = current_pass,
                          .total_passes = total_passes,
                          .percentage = percentage,
                          .status = status ? status : "",
                          .is_complete = is_complete != FALSE,
                          .has_error = has_error != FALSE,
                          .error_message = error_message ? error_message : "",
                          .speed_bytes_per_sec = speed_bytes_per_sec,
                          .estimated_seconds_remaining = estimated_seconds_remaining,
                          .verification_enabled = verification_enabled != FALSE,
                          .verification_in_progress = verification_in_progress != FALSE,
                          .verification_passed = verification_passed != FALSE,
                          .verification_percentage = verification_percentage,
                          .verification_mismatches = 0};

    // Call the callback
    std::lock_guard lock(self->callback_mutex_);
    if (self->progress_callback_) {
        self->progress_callback_(progress);
    }
}

auto DBusClient::get_available_disks() -> std::vector<DiskInfo> {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_)
            return {};
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result = g_dbus_proxy_call_sync(proxy_copy, "GetDisks",
                                              nullptr,  // no parameters
                                              G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS,
                                              nullptr,  // cancellable
                                              &error);

    if (!result) {
        LOG_ERROR("DBusClient",
                  std::format("GetDisks failed: {}", error ? error->message : "unknown"));
        g_clear_error(&error);
        return {};
    }

    std::vector<DiskInfo> disks;

    GVariant* array = g_variant_get_child_value(result, 0);
    GVariantIter iter;
    g_variant_iter_init(&iter, array);

    const gchar* path = nullptr;
    const gchar* model = nullptr;
    const gchar* serial = nullptr;
    gint64 size_bytes = 0;
    gboolean is_removable = FALSE;
    gboolean is_ssd = FALSE;
    const gchar* filesystem = nullptr;
    gboolean is_mounted = FALSE;
    const gchar* mount_point = nullptr;
    guint32 smart_status = 0;

    while (g_variant_iter_next(&iter, "(&s&s&sxbb&sb&su)", &path, &model, &serial, &size_bytes,
                               &is_removable, &is_ssd, &filesystem, &is_mounted, &mount_point,
                               &smart_status)) {
        // Validate required string fields - path is required, others can be empty
        if (!path) {
            LOG_WARNING("DBusClient", "Skipping disk with null path from helper");
            continue;
        }

        SmartData smart;
        smart.status = static_cast<SmartData::HealthStatus>(smart_status);
        smart.available = (smart_status != 0);  // 0 = UNKNOWN means not available

        disks.push_back(DiskInfo{.path = path,
                                 .model = model ? model : "",
                                 .serial = serial ? serial : "",
                                 .size_bytes = static_cast<uint64_t>(size_bytes),
                                 .is_removable = is_removable != FALSE,
                                 .is_ssd = is_ssd != FALSE,
                                 .filesystem = filesystem ? filesystem : "",
                                 .is_mounted = is_mounted != FALSE,
                                 .mount_point = mount_point ? mount_point : "",
                                 .is_lvm_pv = false,  // Not transmitted via D-Bus currently
                                 .smart = smart});
    }

    g_variant_unref(array);
    g_variant_unref(result);

    return disks;
}

auto DBusClient::validate_device_path(const std::string& path) -> std::expected<void, util::Error> {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_) {
            return std::unexpected(util::Error{"Not connected to helper service"});
        }
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result =
        g_dbus_proxy_call_sync(proxy_copy, "ValidateDevicePath", g_variant_new("(s)", path.c_str()),
                               G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        auto msg = std::string(error ? error->message : "unknown error");
        g_clear_error(&error);
        return std::unexpected(util::Error{msg});
    }

    gboolean valid = FALSE;
    const gchar* error_message = nullptr;
    g_variant_get(result, "(b&s)", &valid, &error_message);
    g_variant_unref(result);

    if (!valid) {
        return std::unexpected(util::Error{error_message ? error_message : "Invalid device path"});
    }

    return {};
}

auto DBusClient::is_disk_writable(const std::string& path) -> bool {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_)
            return false;
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result =
        g_dbus_proxy_call_sync(proxy_copy, "IsDeviceWritable", g_variant_new("(s)", path.c_str()),
                               G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        g_clear_error(&error);
        return false;
    }

    gboolean writable = FALSE;
    g_variant_get(result, "(b)", &writable);
    g_variant_unref(result);

    return writable != FALSE;
}

auto DBusClient::get_disk_size(const std::string& path) -> std::expected<uint64_t, util::Error> {
    // Get disk info and extract size
    auto disks = get_available_disks();
    for (const auto& disk : disks) {
        if (disk.path == path) {
            return disk.size_bytes;
        }
    }
    return std::unexpected(util::Error{"Disk not found"});
}

auto DBusClient::unmount_disk(const std::string& path) -> std::expected<void, util::Error> {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_) {
            return std::unexpected(util::Error{"Not connected to helper service"});
        }
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result =
        g_dbus_proxy_call_sync(proxy_copy, "UnmountDevice", g_variant_new("(s)", path.c_str()),
                               G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        auto msg = std::string(error ? error->message : "unknown error");
        g_clear_error(&error);
        return std::unexpected(util::Error{msg});
    }

    gboolean success = FALSE;
    const gchar* error_message = nullptr;
    g_variant_get(result, "(b&s)", &success, &error_message);
    g_variant_unref(result);

    if (!success) {
        return std::unexpected(util::Error{error_message ? error_message : "Unmount failed"});
    }

    return {};
}

void DBusClient::load_algorithms() {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (algorithms_loaded_ || !proxy_)
            return;
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result =
        g_dbus_proxy_call_sync(proxy_copy, "GetAlgorithms", nullptr, G_DBUS_CALL_FLAGS_NONE,
                               DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        g_clear_error(&error);
        return;
    }

    GVariant* array = g_variant_get_child_value(result, 0);
    GVariantIter iter;
    g_variant_iter_init(&iter, array);

    guint32 id = 0;
    const gchar* name = nullptr;
    const gchar* description = nullptr;
    gint pass_count = 0;

    while (g_variant_iter_next(&iter, "(u&s&si)", &id, &name, &description, &pass_count)) {
        algorithms_.emplace(id, AlgorithmInfo{.name = name ? name : "",
                                              .description = description ? description : "",
                                              .pass_count = pass_count});
    }

    g_variant_unref(array);
    g_variant_unref(result);
    algorithms_loaded_ = true;
}

auto DBusClient::wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                           ProgressCallback callback) -> bool {
    return wipe_disk(disk_path, algorithm, std::move(callback), false);
}

auto DBusClient::wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                           ProgressCallback callback, bool verify) -> bool {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_)
            return false;
        proxy_copy = proxy_;
    }

    // Store callback for signal handler
    {
        std::lock_guard lock(callback_mutex_);
        progress_callback_ = std::move(callback);
    }

    GError* error = nullptr;
    GVariant* result = g_dbus_proxy_call_sync(
        proxy_copy, "StartWipe",
        g_variant_new("(sub)", disk_path.c_str(), static_cast<guint32>(algorithm),
                      verify ? TRUE : FALSE),
        G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        LOG_ERROR("DBusClient",
                  std::format("StartWipe failed: {}", error ? error->message : "unknown"));
        g_clear_error(&error);
        return false;
    }

    gboolean started = FALSE;
    const gchar* error_message = nullptr;
    g_variant_get(result, "(b&s)", &started, &error_message);
    g_variant_unref(result);

    if (!started) {
        LOG_ERROR("DBusClient",
                  std::format("Wipe not started: {}", error_message ? error_message : "unknown"));
    }

    return started != FALSE;
}

auto DBusClient::get_algorithm_name(WipeAlgorithm algo) -> std::string {
    load_algorithms();
    if (auto it = algorithms_.find(static_cast<uint32_t>(algo)); it != algorithms_.end()) {
        return it->second.name;
    }
    return "Unknown";
}

auto DBusClient::get_algorithm_description(WipeAlgorithm algo) -> std::string {
    load_algorithms();
    if (auto it = algorithms_.find(static_cast<uint32_t>(algo)); it != algorithms_.end()) {
        return it->second.description;
    }
    return "";
}

auto DBusClient::get_pass_count(WipeAlgorithm algo) -> int {
    load_algorithms();
    if (auto it = algorithms_.find(static_cast<uint32_t>(algo)); it != algorithms_.end()) {
        return it->second.pass_count;
    }
    return 1;
}

auto DBusClient::is_ssd_compatible(WipeAlgorithm algo) -> bool {
    // Multi-pass algorithms are less effective on SSDs
    switch (algo) {
        case WipeAlgorithm::ZERO_FILL:
        case WipeAlgorithm::RANDOM_FILL:
        case WipeAlgorithm::ATA_SECURE_ERASE:
            return true;
        default:
            return false;
    }
}

auto DBusClient::cancel_current_operation() -> bool {
    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_)
            return false;
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result =
        g_dbus_proxy_call_sync(proxy_copy, "CancelWipe", nullptr, G_DBUS_CALL_FLAGS_NONE,
                               DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        g_clear_error(&error);
        return false;
    }

    gboolean cancelled = FALSE;
    g_variant_get(result, "(b)", &cancelled);
    g_variant_unref(result);

    return cancelled != FALSE;
}

auto DBusClient::get_smart_data(const std::string& path) -> SmartData {
    SmartData smart;

    GDBusProxy* proxy_copy = nullptr;
    {
        std::lock_guard lock(proxy_mutex_);
        if (!proxy_)
            return smart;
        proxy_copy = proxy_;
    }

    GError* error = nullptr;
    GVariant* result =
        g_dbus_proxy_call_sync(proxy_copy, "GetDiskSMART", g_variant_new("(s)", path.c_str()),
                               G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS, nullptr, &error);

    if (!result) {
        g_clear_error(&error);
        return smart;
    }

    gboolean available = FALSE;
    gboolean healthy = FALSE;
    gint64 power_on_hours = -1;
    gint32 reallocated_sectors = -1;
    gint32 pending_sectors = -1;
    gint32 temperature_celsius = -1;
    gint32 uncorrectable_errors = -1;
    guint32 status = 0;

    g_variant_get(result, "(bbxiiiiu)", &available, &healthy, &power_on_hours, &reallocated_sectors,
                  &pending_sectors, &temperature_celsius, &uncorrectable_errors, &status);
    g_variant_unref(result);

    smart.available = available != FALSE;
    smart.healthy = healthy != FALSE;
    smart.power_on_hours = power_on_hours;
    smart.reallocated_sectors = reallocated_sectors;
    smart.pending_sectors = pending_sectors;
    smart.temperature_celsius = temperature_celsius;
    smart.uncorrectable_errors = uncorrectable_errors;
    smart.status = static_cast<SmartData::HealthStatus>(status);

    return smart;
}
