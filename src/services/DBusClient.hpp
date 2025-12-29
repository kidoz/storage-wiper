/**
 * @file DBusClient.hpp
 * @brief D-Bus client for communicating with the privileged helper
 *
 * This class provides a client interface to the storage-wiper-helper D-Bus service.
 * It implements both IDiskService and IWipeService interfaces using D-Bus calls.
 */

#pragma once

#include "services/IDiskService.hpp"
#include "services/IWipeService.hpp"
#include "models/DiskInfo.hpp"
#include "models/WipeTypes.hpp"

#include <gio/gio.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    auto wipe_disk(const std::string& disk_path,
                   WipeAlgorithm algorithm,
                   ProgressCallback callback) -> bool override;
    [[nodiscard]] auto get_algorithm_name(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_algorithm_description(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_pass_count(WipeAlgorithm algo) -> int override;
    [[nodiscard]] auto is_ssd_compatible(WipeAlgorithm algo) -> bool override;
    auto cancel_current_operation() -> bool override;

private:
    GDBusConnection* connection_ = nullptr;
    GDBusProxy* proxy_ = nullptr;
    gulong signal_subscription_id_ = 0;
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

    void load_algorithms();
    void setup_signal_handler();
    void cleanup();

    static void on_signal_received(GDBusConnection* connection,
                                   const gchar* sender_name,
                                   const gchar* object_path,
                                   const gchar* interface_name,
                                   const gchar* signal_name,
                                   GVariant* parameters,
                                   gpointer user_data);
};
