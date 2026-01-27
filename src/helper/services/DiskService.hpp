#pragma once

#include "helper/services/SmartService.hpp"
#include "services/IDiskService.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Cached mount information for a single mount point
 */
struct MountEntry {
    std::string device;      // e.g., "/dev/sda1"
    std::string mount_point; // e.g., "/home"
    std::string filesystem;  // e.g., "ext4"
};

/**
 * @brief Cached mount table parsed from /proc/mounts
 */
struct MountCache {
    std::vector<MountEntry> entries;

    // Quick lookup by device path prefix
    [[nodiscard]] auto find_mount_for_device(const std::string& device_path,
                                              const std::vector<std::string>& dm_holders) const
        -> std::optional<MountEntry>;
};

class DiskService : public IDiskService {
public:
    DiskService();
    ~DiskService() override = default;

    // Non-copyable, moveable
    DiskService(const DiskService&) = delete;
    DiskService& operator=(const DiskService&) = delete;
    DiskService(DiskService&&) = default;
    DiskService& operator=(DiskService&&) = default;

    [[nodiscard]] auto get_available_disks() -> std::vector<DiskInfo> override;
    auto unmount_disk(const std::string& path) -> std::expected<void, util::Error> override;
    [[nodiscard]] auto is_disk_writable(const std::string& path) -> bool override;
    [[nodiscard]] auto get_disk_size(const std::string& path)
        -> std::expected<uint64_t, util::Error> override;
    [[nodiscard]] auto validate_device_path(const std::string& path)
        -> std::expected<void, util::Error> override;

    /**
     * @brief Get SMART data for a specific device
     * @param device_path Device path
     * @return SMART data (available=false if not supported)
     */
    [[nodiscard]] auto get_smart_data(const std::string& device_path) -> SmartData;

    /**
     * @brief Clear the disk list cache (call when devices change)
     */
    void invalidate_cache();

private:
    /**
     * @brief Parse disk info without SMART data (fast path)
     * @param device_path Device path
     * @param mount_cache Pre-parsed mount table
     * @return DiskInfo with smart.available = false
     */
    [[nodiscard]] auto parse_disk_info(const std::string& device_path,
                                        const MountCache& mount_cache) -> DiskInfo;

    [[nodiscard]] auto check_if_ssd(const std::string& device_path) -> bool;

    /**
     * @brief Parse /proc/mounts once into a cache structure
     * @return Parsed mount entries
     */
    [[nodiscard]] static auto parse_mount_table() -> MountCache;

    /**
     * @brief Collect dm-* holders for a device and its partitions
     * @param sys_path Path in /sys/block/
     * @param device_name Device name (e.g., "sda")
     * @return List of dm-* device names
     */
    [[nodiscard]] static auto collect_dm_holders(const std::string& sys_path,
                                                  const std::string& device_name)
        -> std::vector<std::string>;

    std::unique_ptr<SmartService> smart_service_;

    // Result cache with TTL
    mutable std::mutex cache_mutex_;
    std::vector<DiskInfo> cached_disks_;
    std::chrono::steady_clock::time_point cache_timestamp_;
    static constexpr auto CACHE_TTL = std::chrono::milliseconds{500};
};
