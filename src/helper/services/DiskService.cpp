#include "helper/services/DiskService.hpp"

#include "helper/services/DevicePathMatcher.hpp"
#include "helper/services/SmartService.hpp"
#include "util/FileDescriptor.hpp"
#include "util/Logger.hpp"

// Standard library
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

// System headers
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mntent.h>

// Linux-specific headers
#include <linux/fs.h>

namespace fs = std::filesystem;
namespace rng = std::ranges;

namespace {
constexpr auto BYTES_PER_SECTOR = uint64_t{512};

// Virtual device name prefixes to skip. Matched with starts_with: substring
// matching would silently drop any future device name embedding these tokens.
constexpr std::array VIRTUAL_PREFIXES{"loop", "ram", "dm-", "zram", "md", "nbd"};

auto is_virtual_device(std::string_view name) noexcept -> bool {
    return rng::any_of(VIRTUAL_PREFIXES,
                       [name](const char* prefix) { return name.starts_with(prefix); });
}

}  // namespace

// ============================================================================
// MountCache implementation
// ============================================================================

auto MountCache::find_mount_for_device(const std::string& device_path,
                                       const std::vector<std::string>& dm_holders) const
    -> std::optional<MountEntry> {
    // First, check direct mount of device or its partitions
    for (const auto& entry : entries) {
        if (device_path_matcher::is_device_or_partition_of(device_path, entry.device)) {
            return entry;
        }
    }

    // Check if any dm-* holder is mounted
    for (const auto& dm_name : dm_holders) {
        const auto dm_path = std::format("/dev/{}", dm_name);
        for (const auto& entry : entries) {
            if (entry.device == dm_path) {
                return entry;
            }
        }
    }

    // Check /dev/mapper/* entries by resolving symlinks
    if (!dm_holders.empty()) {
        for (const auto& entry : entries) {
            if (entry.device.starts_with("/dev/mapper/")) {
                std::error_code ec;
                const auto real_path = fs::read_symlink(entry.device, ec);
                if (!ec) {
                    const auto resolved_dm_name = real_path.filename().string();
                    if (rng::find(dm_holders, resolved_dm_name) != dm_holders.end()) {
                        return entry;
                    }
                }
            }
        }
    }

    return std::nullopt;
}

// ============================================================================
// DiskService implementation
// ============================================================================

DiskService::DiskService() : smart_service_(std::make_shared<SmartService>()) {}

auto DiskService::get_smart_data(const std::string& device_path) -> SmartData {
    if (!smart_service_) {
        return {};
    }
    return smart_service_->get_smart_data(device_path);
}

void DiskService::invalidate_cache() {
    std::lock_guard lock{cache_mutex_};
    cached_disks_.clear();
    cache_timestamp_ = {};
}

auto DiskService::get_available_disks_sync() -> std::vector<DiskInfo> {
    // Check cache first (with TTL)
    {
        std::lock_guard lock{cache_mutex_};
        const auto now = std::chrono::steady_clock::now();
        if (!cached_disks_.empty() && (now - cache_timestamp_) < CACHE_TTL) {
            return cached_disks_;
        }
    }

    const fs::path block_dir{"/sys/block"};

    if (!fs::exists(block_dir)) {
        return {};
    }

    // OPTIMIZATION 1: Parse /proc/mounts once for all disks
    const auto mount_cache = parse_mount_table();

    // OPTIMIZATION 2: First pass - collect basic disk info (fast, no SMART)
    std::vector<DiskInfo> disks;
    std::vector<std::string> smart_eligible_paths;

    for (const auto& entry : fs::directory_iterator{block_dir}) {
        const auto device_name = entry.path().filename().string();

        if (is_virtual_device(device_name)) {
            continue;
        }

        const auto device_path = std::format("/dev/{}", device_name);

        if (auto valid = validate_device_path(device_path); !valid) {
            continue;
        }

        if (auto info = parse_disk_info(device_path, mount_cache); info.size_bytes > 0) {
            // Track paths that need SMART data
            if (smart_service_ && SmartService::is_smart_supported(device_path)) {
                smart_eligible_paths.push_back(device_path);
            }
            disks.emplace_back(std::move(info));
        }
    }

    // OPTIMIZATION 3: Parallel SMART collection using std::thread with timeout
    if (!smart_eligible_paths.empty() && smart_service_) {
        std::vector<std::future<std::pair<std::string, SmartData>>> smart_futures;
        smart_futures.reserve(smart_eligible_paths.size());

        // Capture the shared_ptr by value: a query thread that overruns the
        // 250ms timeout below is detached and may outlive this DiskService.
        std::shared_ptr<SmartService> smart_service_ptr = smart_service_;
        for (const auto& path : smart_eligible_paths) {
            auto task = std::make_shared<std::packaged_task<std::pair<std::string, SmartData>()>>(
                [smart_service_ptr, path]() {
                    return std::make_pair(path, smart_service_ptr->get_smart_data(path));
                });
            smart_futures.push_back(task->get_future());
            std::thread([task]() { (*task)(); }).detach();
        }

        // Collect results with a strict timeout to prevent hanging on slow drives
        std::unordered_map<std::string, SmartData> smart_results;
        for (auto& future : smart_futures) {
            try {
                // Wait max 250ms per drive (parallelized, so total max ~250ms)
                if (future.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready) {
                    auto [path, data] = future.get();
                    smart_results[path] = std::move(data);
                } else {
                    LOG_WARNING("DiskService", "SMART data query timed out for a device");
                }
            } catch (const std::system_error& e) {
                LOG_WARNING("DiskService", std::format("System error reading SMART: {}", e.what()));
            } catch (const std::exception& e) {
                LOG_WARNING("DiskService", std::format("Failed to read SMART: {}", e.what()));
            } catch (...) {
                LOG_WARNING("DiskService", "Unknown error reading SMART");
            }
        }

        // Apply SMART data to disks
        for (auto& disk : disks) {
            if (auto it = smart_results.find(disk.path); it != smart_results.end()) {
                disk.smart = std::move(it->second);
            }
        }
    }

    // Update cache
    {
        std::lock_guard lock{cache_mutex_};
        cached_disks_ = disks;
        cache_timestamp_ = std::chrono::steady_clock::now();
    }

    return disks;
}

auto DiskService::get_available_disks_blocking()
    -> std::expected<std::vector<DiskInfo>, util::Error> {
    return get_available_disks_sync();
}

void DiskService::get_available_disks(
    std::function<void(std::expected<std::vector<DiskInfo>, util::Error>)> callback) {
    if (callback) {
        callback(get_available_disks_sync());
    }
}

auto DiskService::parse_mount_table() -> MountCache {
    MountCache cache;

    auto mtab_deleter = [](FILE* f) {
        if (f)
            ::endmntent(f);
    };

    std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"),
                                                       mtab_deleter};
    if (!mtab) {
        return cache;
    }

    while (auto* entry = ::getmntent(mtab.get())) {
        cache.entries.push_back(MountEntry{
            .device = entry->mnt_fsname,
            .mount_point = entry->mnt_dir,
            .filesystem = entry->mnt_type,
        });
    }

    return cache;
}

auto DiskService::collect_dm_holders(const std::string& sys_path, const std::string& device_name)
    -> std::vector<std::string> {
    std::vector<std::string> dm_holders;

    auto collect_from_path = [&dm_holders](const fs::path& holders_path) {
        if (!fs::exists(holders_path)) {
            return;
        }
        std::error_code ec;
        for (const auto& holder : fs::directory_iterator{holders_path, ec}) {
            if (ec)
                break;
            const auto holder_name = holder.path().filename().string();
            if (holder_name.starts_with("dm-")) {
                dm_holders.push_back(holder_name);
            }
        }
    };

    // Check holders of the device itself
    collect_from_path(sys_path + "/holders");

    // Also check holders of partitions (e.g., /dev/nvme0n1p1 -> dm-0)
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator{sys_path, ec}) {
        if (ec)
            break;
        const auto part_name = entry.path().filename().string();
        if (part_name.starts_with(device_name) && part_name != device_name) {
            collect_from_path(entry.path() / "holders");
        }
    }

    return dm_holders;
}

auto DiskService::unmount_disk(const std::string& path) -> std::expected<void, util::Error> {
    if (auto valid = validate_device_path(path); !valid) {
        return std::unexpected(valid.error());
    }

    // Collect all mount points for this device and its partitions
    std::vector<std::string> mount_points;

    if (auto mtab_deleter =
            [](FILE* f) {
                if (f)
                    ::endmntent(f);
            };
        std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"),
                                                           mtab_deleter}) {
        while (auto* entry = ::getmntent(mtab.get())) {
            const std::string_view mount_device{entry->mnt_fsname};

            // Match device itself or any partition (e.g., /dev/sda, /dev/sda1, /dev/sda2).
            // Suffix check guards against /dev/sdaa1 leaking into a /dev/sda match.
            if (device_path_matcher::is_device_or_partition_of(path, mount_device)) {
                mount_points.push_back(entry->mnt_dir);
            }
        }
    }

    if (mount_points.empty()) {
        // Nothing mounted - success. Still invalidate: the cache may hold a
        // stale mounted=true entry from before an external unmount.
        invalidate_cache();
        return {};
    }

    // Unmount in reverse order (nested mounts)
    std::ranges::reverse(mount_points);

    std::string failed_mount;
    int last_errno = 0;

    for (const auto& mount_point : mount_points) {
        // Use normal umount and fail if busy to ensure data consistency
        if (::umount(mount_point.c_str()) != 0) {
            last_errno = errno;
            failed_mount = mount_point;
            // Continue trying other mount points, but we will likely fail later
        }
    }

    // Check if anything is still mounted
    if (auto mtab_deleter =
            [](FILE* f) {
                if (f)
                    ::endmntent(f);
            };
        std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"),
                                                           mtab_deleter}) {
        while (auto* entry = ::getmntent(mtab.get())) {
            const std::string_view mount_device{entry->mnt_fsname};

            if (device_path_matcher::is_device_or_partition_of(path, mount_device)) {
                // Still mounted. Report the mount point whose umount() actually
                // failed, so the errno matches the path in the message.
                const std::string error_str =
                    last_errno ? std::strerror(last_errno) : "Device busy";
                const std::string mount_point =
                    failed_mount.empty() ? std::string{entry->mnt_dir} : failed_mount;
                return std::unexpected(util::Error{
                    std::format("Failed to unmount {}: {}", mount_point, error_str), last_errno});
            }
        }
    }

    // Invalidate cache since mount status changed
    invalidate_cache();

    return {};
}

auto DiskService::is_disk_writable(const std::string& path) -> bool {
    if (auto valid = validate_device_path(path); !valid) {
        return false;
    }

    const util::FileDescriptor fd{::open(path.c_str(), O_RDWR)};
    return fd.is_valid();
}

auto DiskService::get_disk_size(const std::string& path) -> std::expected<uint64_t, util::Error> {
    if (auto valid = validate_device_path(path); !valid) {
        return std::unexpected(valid.error());
    }

    const util::FileDescriptor fd{::open(path.c_str(), O_RDONLY)};
    if (!fd) {
        return std::unexpected(
            util::Error{std::format("Failed to open device: {}", std::strerror(errno)), errno});
    }

    uint64_t size{};
    if (::ioctl(fd.get(), BLKGETSIZE64, &size) != 0) {
        return std::unexpected(util::Error{
            std::format("Failed to query device size: {}", std::strerror(errno)), errno});
    }
    return size;
}

auto DiskService::validate_device_path(const std::string& path)
    -> std::expected<void, util::Error> {
    // Whitelist of allowed device prefixes (physical disks only)
    // Explicitly excludes /dev/mapper/* and /dev/dm-* (LVM logical volumes)
    // Physical disks that are LVM Physical Volumes (PVs) ARE allowed
    constexpr std::array allowed_prefixes{
        std::string_view{"/dev/sd"},      // SATA/SCSI disks
        std::string_view{"/dev/nvme"},    // NVMe drives
        std::string_view{"/dev/mmcblk"},  // MMC/SD cards
        std::string_view{"/dev/vd"}       // Virtual disks (VMs)
    };

    const auto has_allowed_prefix = [&allowed_prefixes](std::string_view candidate) noexcept {
        return rng::any_of(allowed_prefixes, [candidate](std::string_view prefix) noexcept {
            return candidate.starts_with(prefix);
        });
    };

    if (!has_allowed_prefix(path)) {
        return std::unexpected(util::Error{"Device path prefix not allowed"});
    }

    // Re-check the resolved target: stat() follows symlinks, so a link with an
    // allowed name (/dev/sdx -> /dev/dm-0) would otherwise smuggle an excluded
    // device-mapper node past the prefix filter.
    std::error_code ec;
    const auto canonical = fs::canonical(path, ec);
    if (ec) {
        return std::unexpected(util::Error{
            std::format("Failed to canonicalize device path: {}", ec.message()), ec.value()});
    }

    const std::string canonical_path = canonical.string();
    if (!has_allowed_prefix(canonical_path)) {
        return std::unexpected(util::Error{"Device path resolves to a disallowed device"});
    }

    // Verify it's actually a block device
    struct stat st{};
    if (::stat(canonical_path.c_str(), &st) != 0) {
        return std::unexpected(util::Error{
            std::format("Failed to stat device path: {}", std::strerror(errno)), errno});
    }
    if (!S_ISBLK(st.st_mode)) {
        return std::unexpected(util::Error{"Device path is not a block device"});
    }
    return {};
}

auto DiskService::parse_disk_info(const std::string& device_path, const MountCache& mount_cache)
    -> DiskInfo {
    auto info = DiskInfo{.path = device_path,
                         .model = {},
                         .serial = {},
                         .size_bytes = 0,
                         .is_removable = false,
                         .is_ssd = false,
                         .filesystem = {},
                         .is_mounted = false,
                         .mount_point = {},
                         .is_lvm_pv = false,
                         .smart = {}};

    const auto device_name = fs::path{device_path}.filename().string();
    const auto sys_path = std::format("/sys/block/{}", device_name);

    // Helper lambdas for reading different types from sysfs
    auto read_uint64 = [](const fs::path& path) -> std::optional<uint64_t> {
        if (std::ifstream file{path}; file.is_open()) {
            uint64_t value{};
            if (file >> value) {
                return value;
            }
        }
        return std::nullopt;
    };

    auto read_int = [](const fs::path& path) -> std::optional<int> {
        if (std::ifstream file{path}; file.is_open()) {
            int value{};
            if (file >> value) {
                return value;
            }
        }
        return std::nullopt;
    };

    // Get disk size in sectors and convert to bytes
    // Add overflow check to prevent arithmetic overflow on extremely large disks
    if (const auto sectors = read_uint64(sys_path + "/size")) {
        constexpr auto MAX_SECTORS = UINT64_MAX / BYTES_PER_SECTOR;
        if (*sectors <= MAX_SECTORS) {
            info.size_bytes = *sectors * BYTES_PER_SECTOR;
        }
    }

    // Get model name with automatic whitespace trimming
    if (std::ifstream model_file{sys_path + "/device/model"}) {
        std::getline(model_file, info.model);
        info.model = std::string{
            std::string_view{info.model}.substr(0, info.model.find_last_not_of(" \n\r\t") + 1)};
    }

    // Get serial number (exposed by NVMe and some SATA/USB bridges; best-effort)
    if (std::ifstream serial_file{sys_path + "/device/serial"}) {
        std::getline(serial_file, info.serial);
        info.serial = std::string{
            std::string_view{info.serial}.substr(0, info.serial.find_last_not_of(" \n\r\t") + 1)};
    }

    // Check if removable
    if (const auto removable = read_int(sys_path + "/removable")) {
        info.is_removable = (*removable == 1);
    }

    info.is_ssd = check_if_ssd(device_path);

    // Collect device-mapper (dm-*) holders for this device and its partitions
    auto dm_holders = collect_dm_holders(sys_path, device_name);
    info.is_lvm_pv = !dm_holders.empty();

    // OPTIMIZATION: Use pre-parsed mount cache instead of re-reading /proc/mounts
    if (auto mount = mount_cache.find_mount_for_device(device_path, dm_holders)) {
        info.is_mounted = true;
        info.mount_point = mount->mount_point;
        info.filesystem = mount->filesystem;
    }

    // NOTE: SMART data is NOT collected here anymore - it's done in parallel
    // in get_available_disks() for better performance

    return info;
}

auto DiskService::check_if_ssd(const std::string& device_path) -> bool {
    const auto device_name = fs::path{device_path}.filename().string();
    const auto rotational_path = std::format("/sys/block/{}/queue/rotational", device_name);

    // Check the rotational flag for this device
    // For physical disks (sd*, nvme*, etc.), this directly indicates SSD vs HDD
    // Note: This works correctly for physical disks even if they're used as LVM PVs
    if (std::ifstream file{rotational_path}; file.is_open()) {
        int rotational{};
        if (file >> rotational) {
            return rotational == 0;  // SSD if not rotational
        }
    }

    return false;  // Default to HDD if unknown
}
