/**
 * @file DBusClient.hpp
 * @brief D-Bus client for communicating with the privileged helper
 *
 * This class provides a client interface to the storage-wiper-helper D-Bus service.
 * It implements both IDiskService and IWipeService interfaces using D-Bus calls.
 */

#pragma once

#include "models/DiskInfo.hpp"
#include "models/WipeTypes.hpp"
#include "services/IDiskService.hpp"
#include "services/IWipeService.hpp"

#include <gio/gio.h>

#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

/**
 * @enum ConnectionState
 * @brief Connection state for D-Bus client
 */
enum class ConnectionState {
    DISCONNECTED,  ///< No connection, not trying to connect
    CONNECTING,    ///< Currently attempting to connect
    CONNECTED,     ///< Successfully connected
    RECONNECTING,  ///< Lost connection, attempting to reconnect
    FAILED         ///< Permanent failure (max retries exceeded)
};

/**
 * @class DBusClient
 * @brief Client for the storage-wiper-helper D-Bus service
 *
 * Provides disk listing and wiping operations via D-Bus to the privileged helper.
 * Progress is received via D-Bus signals.
 */
class DBusClient : public IDiskService, public IWipeService {
public:
    DBusClient();
    ~DBusClient() override;

    // Prevent copying
    DBusClient(const DBusClient&) = delete;
    DBusClient& operator=(const DBusClient&) = delete;
    DBusClient(DBusClient&&) = delete;
    DBusClient& operator=(DBusClient&&) = delete;

    /**
     * @brief Connect to the D-Bus helper service
     * @return true if connected successfully
     */
    [[nodiscard]] auto connect() -> bool;

    /**
     * @brief Check if connected to the helper
     * @return true if connected
     */
    [[nodiscard]] auto is_connected() const -> bool;

    /**
     * @brief Get current connection state
     * @return Current ConnectionState
     */
    [[nodiscard]] auto get_connection_state() const -> ConnectionState;

    /**
     * @brief Set callback for connection state changes
     * @param callback Function called when state changes (state, error_message)
     *
     * Thread-safe. The callback is invoked on the GLib main loop thread.
     */
    using ConnectionStateCallback = std::function<void(ConnectionState, const std::string&)>;
    void set_connection_state_callback(ConnectionStateCallback callback);

    /**
     * @brief Manually trigger a reconnection attempt
     * @return true if reconnection started, false if already connected/connecting
     */
    auto request_reconnect() -> bool;

    /**
     * @brief Check if the helper service is available on D-Bus
     * @return true if service name has an owner
     */
    [[nodiscard]] auto is_service_available() const -> bool;

    // IDiskService interface
    [[nodiscard]] auto get_available_disks() -> std::vector<DiskInfo> override;
    [[nodiscard]] auto validate_device_path(const std::string& path)
        -> std::expected<void, util::Error> override;
    [[nodiscard]] auto is_disk_writable(const std::string& path) -> bool override;
    [[nodiscard]] auto get_disk_size(const std::string& path)
        -> std::expected<uint64_t, util::Error> override;
    [[nodiscard]] auto unmount_disk(const std::string& path)
        -> std::expected<void, util::Error> override;

    // IWipeService interface
    auto wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm, ProgressCallback callback)
        -> bool override;
    [[nodiscard]] auto get_algorithm_name(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_algorithm_description(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_pass_count(WipeAlgorithm algo) -> int override;
    [[nodiscard]] auto is_ssd_compatible(WipeAlgorithm algo) -> bool override;
    auto cancel_current_operation() -> bool override;

private:
    GDBusConnection* connection_ = nullptr;
    GDBusProxy* proxy_ = nullptr;
    guint signal_subscription_id_ = 0;
    ProgressCallback progress_callback_;
    mutable std::mutex callback_mutex_;

    // Algorithm info cache (fetched once from helper)
    struct AlgorithmInfo {
        uint32_t id;
        std::string name;
        std::string description;
        int pass_count;
    };
    std::vector<AlgorithmInfo> algorithms_;
    bool algorithms_loaded_ = false;

    // Connection state management
    ConnectionState connection_state_ = ConnectionState::DISCONNECTED;
    mutable std::mutex state_mutex_;

    // Reconnection configuration
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;
    static constexpr int INITIAL_RETRY_DELAY_MS = 500;
    static constexpr int MAX_RETRY_DELAY_MS = 30'000;
    int reconnect_attempts_ = 0;
    guint reconnect_timer_id_ = 0;

    // Service name watching
    guint name_watcher_id_ = 0;

    // Connection state callback
    ConnectionStateCallback state_callback_;
    std::mutex state_callback_mutex_;

    // Random number generator for jitter
    std::mt19937 rng_{std::random_device{}()};

    void load_algorithms();
    void setup_signal_handler();
    void cleanup();

    // State management
    void set_state(ConnectionState new_state, const std::string& error_message = "");
    void notify_state_change(ConnectionState state, const std::string& message);

    // Reconnection logic
    void schedule_reconnect();
    auto attempt_reconnect() -> bool;
    void reset_reconnect_state();
    [[nodiscard]] auto get_retry_delay_ms() -> int;

    // Service availability monitoring
    void start_name_watching();
    void stop_name_watching();

    // Static callbacks
    static void on_signal_received(GDBusConnection* connection, const gchar* sender_name,
                                   const gchar* object_path, const gchar* interface_name,
                                   const gchar* signal_name, GVariant* parameters,
                                   gpointer user_data);

    static gboolean on_reconnect_timer(gpointer user_data);

    static void on_name_appeared(GDBusConnection* connection, const gchar* name,
                                 const gchar* name_owner, gpointer user_data);

    static void on_name_vanished(GDBusConnection* connection, const gchar* name,
                                 gpointer user_data);
};
