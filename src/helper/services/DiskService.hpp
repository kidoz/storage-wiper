#pragma once

#include "helper/services/SmartService.hpp"
#include "services/IDiskService.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Cached mount information for a single mount point
 */
struct MountEntry {
    std::string device;       // e.g., "/dev/sda1"
    std::string mount_point;  // e.g., "/home"
    std::string filesystem;   // e.g., "ext4"
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

/**
 * @brief Health data collected by SMART queries that outran their caller
 *
 * A query against a sleeping drive can take far longer than a disk enumeration
 * is willing to wait. Rather than discard that work, the query thread deposits
 * its result here and the next enumeration picks it up. The state also tracks
 * which devices have a query in flight so repeated refreshes do not pile up
 * threads on a device that is not answering.
 *
 * Shared by detached threads, so every member is guarded by the mutex and the
 * object is owned through a shared_ptr that outlives the service.
 */
struct SmartQueryState {
    /**
     * @brief Claim a device for querying
     * @param device_path Device to query
     * @return true if the caller should start a query, false if one is already running
     */
    [[nodiscard]] auto try_begin_query(const std::string& device_path) -> bool;

    /**
     * @brief Record a completed query result and release the device
     * @param device_path Device that was queried
     * @param data Result to cache
     */
    void finish_query(const std::string& device_path, SmartData data);

    /**
     * @brief Look up the most recent result for a device
     * @param device_path Device to look up
     * @return Cached data, or nullopt if no query has completed yet
     */
    [[nodiscard]] auto lookup(const std::string& device_path) const -> std::optional<SmartData>;

    /**
     * @brief Drop cached results for devices that are no longer present
     * @param present_paths Device paths seen by the current enumeration
     */
    void retain(const std::vector<std::string>& present_paths);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SmartData> results_;
    std::unordered_set<std::string> in_flight_;
};

class DiskService : public IDiskService {
public:
    DiskService();
    ~DiskService() override = default;

    // Non-copyable and non-movable due to mutex member
    DiskService(const DiskService&) = delete;
    DiskService& operator=(const DiskService&) = delete;
    DiskService(DiskService&&) = delete;
    DiskService& operator=(DiskService&&) = delete;

    void get_available_disks(
        std::function<void(std::expected<std::vector<DiskInfo>, util::Error>)> callback) override;

    [[nodiscard]] auto get_available_disks_blocking()
        -> std::expected<std::vector<DiskInfo>, util::Error> override;

    // Helper method for synchronous access within the daemon process (not part of IDiskService)
    [[nodiscard]] auto get_available_disks_sync() -> std::vector<DiskInfo>;
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
    void invalidate_cache() override;

private:
    /**
     * @brief Parse disk info without SMART data (fast path)
     * @param device_path Device path
     * @param mount_cache Pre-parsed mount table
     * @return DiskInfo with smart.available = false
     */
    [[nodiscard]] auto parse_disk_info(const std::string& device_path,
                                       const MountCache& mount_cache) -> DiskInfo;

    /**
     * @brief Parse one partition of a disk (fast path)
     *
     * Inherits model, serial, SSD flag, and removability from the parent disk;
     * size, mount state, and LVM holder status are read for the partition.
     *
     * @param device_path Partition device path (e.g., /dev/sda1)
     * @param part_sys_path sysfs directory of the partition
     * @param parent Parent disk info (as returned by parse_disk_info)
     * @param mount_cache Pre-parsed mount table
     * @return DiskInfo with is_partition = true
     */
    [[nodiscard]] static auto parse_partition_info(const std::string& device_path,
                                                   const std::string& part_sys_path,
                                                   const DiskInfo& parent,
                                                   const MountCache& mount_cache) -> DiskInfo;

    /**
     * @brief Append all partitions of a disk to the disk list
     * @param disks List to append to; the parent disk must be disks.back()
     * @param disk_sys_path sysfs directory of the parent disk
     * @param mount_cache Pre-parsed mount table
     */
    void append_partitions(std::vector<DiskInfo>& disks, const std::string& disk_sys_path,
                           const MountCache& mount_cache);

    /**
     * @brief Check whether a device name is a partition of some disk
     * @param device_name Kernel device name (e.g., "sda1")
     * @return true when the sysfs "partition" attribute exists for the device
     */
    [[nodiscard]] static auto is_partition_device(const std::string& device_name) -> bool;

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

    // shared_ptr: detached SMART query threads that outlive the collection
    // budget capture these, so they must survive DiskService destruction.
    std::shared_ptr<SmartService> smart_service_;
    std::shared_ptr<SmartQueryState> smart_state_;

    // Result cache with TTL
    mutable std::mutex cache_mutex_;
    std::vector<DiskInfo> cached_disks_;
    std::chrono::steady_clock::time_point cache_timestamp_;
    static constexpr auto CACHE_TTL = std::chrono::milliseconds{500};
};
