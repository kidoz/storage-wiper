#include "helper/services/DiskService.hpp"
#include "util/FileDescriptor.hpp"

// Standard library
#include <algorithm>
#include <array>
#include <vector>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

// System headers
#include <fcntl.h>
#include <mntent.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

// Linux-specific headers
#include <linux/fs.h>

namespace fs = std::filesystem;
namespace rng = std::ranges;

namespace {
    constexpr auto BYTES_PER_SECTOR = uint64_t{512};

    auto is_partition_suffix(std::string_view suffix) noexcept -> bool {
        if (suffix.empty()) {
            return false;
        }
        const auto first = static_cast<unsigned char>(suffix.front());
        return std::isdigit(first) || suffix.front() == 'p';
    }
}

auto DiskService::get_available_disks() -> std::vector<DiskInfo> {
    const fs::path block_dir{"/sys/block"};
    
    if (!fs::exists(block_dir)) {
        return {};
    }
    
    // Virtual device patterns to skip
    // Note: dm- devices (LVM/device-mapper) are filtered here in /sys/block
    // but physical disks that are LVM PVs are still shown (e.g., /dev/sda)
    constexpr std::array virtual_patterns{"loop", "ram", "dm-"};

    auto is_virtual_device = [&virtual_patterns](std::string_view name) noexcept {
        return rng::any_of(virtual_patterns, [name](const char* pattern) {
            return name.contains(pattern);
        });
    };
    
    std::vector<DiskInfo> disks;
    
    for (const auto& entry : fs::directory_iterator{block_dir}) {
        const auto device_name = entry.path().filename().string();
        
        if (is_virtual_device(device_name)) {
            continue;
        }
        
        const auto device_path = std::format("/dev/{}", device_name);
        
        if (auto valid = validate_device_path(device_path); !valid) {
            continue;
        }
        
        if (auto info = parse_disk_info(device_path); info.size_bytes > 0) {
            disks.emplace_back(std::move(info));
        }
    }
    
    return disks;
}

auto DiskService::unmount_disk(const std::string& path) -> std::expected<void, util::Error> {
    if (auto valid = validate_device_path(path); !valid) {
        return std::unexpected(valid.error());
    }

    // Collect all mount points for this device and its partitions
    std::vector<std::string> mount_points;

    if (auto mtab_deleter = [](FILE* f) { if (f) ::endmntent(f); };
        std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"), mtab_deleter}) {

        while (auto* entry = ::getmntent(mtab.get())) {
            const std::string_view mount_device{entry->mnt_fsname};

            // Match device itself or any partition (e.g., /dev/sda, /dev/sda1, /dev/sda2)
            if (mount_device == path ||
                (mount_device.starts_with(path) && mount_device.size() > path.size())) {
                mount_points.push_back(entry->mnt_dir);
            }
        }
    }

    if (mount_points.empty()) {
        // Nothing mounted - success
        return {};
    }

    // Unmount in reverse order (nested mounts)
    std::ranges::reverse(mount_points);

    std::string failed_mount;
    int last_errno = 0;

    for (const auto& mount_point : mount_points) {
        // Try lazy unmount (MNT_DETACH) - most reliable for busy filesystems
        if (::umount2(mount_point.c_str(), MNT_DETACH) != 0) {
            // Try force unmount as fallback
            if (::umount2(mount_point.c_str(), MNT_FORCE) != 0) {
                last_errno = errno;
                failed_mount = mount_point;
                // Continue trying other mount points
            }
        }
    }

    // Check if anything is still mounted
    if (auto mtab_deleter = [](FILE* f) { if (f) ::endmntent(f); };
        std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"), mtab_deleter}) {

        while (auto* entry = ::getmntent(mtab.get())) {
            const std::string_view mount_device{entry->mnt_fsname};

            if (mount_device == path ||
                (mount_device.starts_with(path) && mount_device.size() > path.size())) {
                // Still mounted
                const std::string error_str = last_errno ? std::strerror(last_errno) : "Device busy";
                return std::unexpected(util::Error{
                    std::format("Failed to unmount {}: {}", entry->mnt_dir, error_str), last_errno});
            }
        }
    }

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
        return std::unexpected(util::Error{
            std::format("Failed to open device: {}", std::strerror(errno)), errno});
    }

    uint64_t size{};
    if (::ioctl(fd.get(), BLKGETSIZE64, &size) != 0) {
        return std::unexpected(util::Error{
            std::format("Failed to query device size: {}", std::strerror(errno)), errno});
    }
    return size;
}

auto DiskService::validate_device_path(const std::string& path) -> std::expected<void, util::Error> {
    // Whitelist of allowed device prefixes (physical disks only)
    // Explicitly excludes /dev/mapper/* and /dev/dm-* (LVM logical volumes)
    // Physical disks that are LVM Physical Volumes (PVs) ARE allowed
    constexpr std::array allowed_prefixes{
        std::string_view{"/dev/sd"},      // SATA/SCSI disks
        std::string_view{"/dev/nvme"},    // NVMe drives
        std::string_view{"/dev/mmcblk"},  // MMC/SD cards
        std::string_view{"/dev/vd"}       // Virtual disks (VMs)
    };

    const std::string_view path_view{path};

    const auto has_valid_prefix = rng::any_of(allowed_prefixes,
        [path_view](std::string_view prefix) noexcept {
            return path_view.starts_with(prefix);
        });

    if (!has_valid_prefix) {
        return std::unexpected(util::Error{"Device path prefix not allowed"});
    }

    // Verify it's actually a block device
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return std::unexpected(util::Error{
            std::format("Failed to stat device path: {}", std::strerror(errno)), errno});
    }
    if (!S_ISBLK(st.st_mode)) {
        return std::unexpected(util::Error{"Device path is not a block device"});
    }
    return {};
}

auto DiskService::parse_disk_info(const std::string& device_path) -> DiskInfo {
    auto info = DiskInfo{
        .path = device_path,
        .model = {},
        .serial = {},
        .size_bytes = 0,
        .is_removable = false,
        .is_ssd = false,
        .filesystem = {},
        .is_mounted = false,
        .mount_point = {},
        .is_lvm_pv = false
    };
    
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
        info.model = std::string{std::string_view{info.model}.substr(0, info.model.find_last_not_of(" \n\r\t") + 1)};
    }
    
    // Check if removable
    if (const auto removable = read_int(sys_path + "/removable")) {
        info.is_removable = (*removable == 1);
    }
    
    info.is_ssd = check_if_ssd(device_path);

    // Collect device-mapper (dm-*) holders for this device and its partitions
    // This detects LVM, LUKS, and other dm-based setups
    std::vector<std::string> dm_holders;

    auto collect_holders = [&dm_holders](const fs::path& holders_path) {
        if (!fs::exists(holders_path)) {
            return;
        }
        for (const auto& holder : fs::directory_iterator{holders_path}) {
            const auto holder_name = holder.path().filename().string();
            if (holder_name.starts_with("dm-")) {
                dm_holders.push_back(holder_name);
            }
        }
    };

    // Check holders of the device itself
    collect_holders(sys_path + "/holders");

    // Also check holders of partitions (e.g., /dev/nvme0n1p1 -> dm-0)
    for (const auto& entry : fs::directory_iterator{sys_path}) {
        const auto part_name = entry.path().filename().string();
        if (part_name.starts_with(device_name) && part_name != device_name) {
            collect_holders(entry.path() / "holders");
        }
    }

    info.is_lvm_pv = !dm_holders.empty();

    // Check mount status using RAII wrapper
    if (auto mtab_deleter = [](FILE* f) { if (f) ::endmntent(f); };
        std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"), mtab_deleter}) {

        while (auto* entry = ::getmntent(mtab.get())) {
            const std::string_view mount_device{entry->mnt_fsname};

            // Check direct mount of device or its partitions
            if (mount_device == device_path ||
                (mount_device.starts_with(device_path) &&
                 is_partition_suffix(mount_device.substr(device_path.size())))) {
                info.is_mounted = true;
                info.mount_point = entry->mnt_dir;
                info.filesystem = entry->mnt_type;
                break;
            }

            // Check if any dm-* holder is mounted (LVM/LUKS)
            for (const auto& dm_name : dm_holders) {
                const auto dm_path = std::format("/dev/{}", dm_name);
                if (mount_device == dm_path || mount_device.starts_with("/dev/mapper/")) {
                    // For /dev/mapper/* entries, resolve to check if it's our dm device
                    // by checking if the mounted device's dm name matches
                    if (mount_device == dm_path) {
                        info.is_mounted = true;
                        info.mount_point = entry->mnt_dir;
                        info.filesystem = entry->mnt_type;
                        break;
                    }
                }
            }
            if (info.is_mounted) break;
        }
    }

    // If we have dm holders but couldn't match mount by dm-* path,
    // try matching by resolving /dev/mapper/* symlinks
    if (!info.is_mounted && !dm_holders.empty()) {
        if (auto mtab_deleter = [](FILE* f) { if (f) ::endmntent(f); };
            std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"), mtab_deleter}) {

            while (auto* entry = ::getmntent(mtab.get())) {
                const std::string_view mount_device{entry->mnt_fsname};

                if (mount_device.starts_with("/dev/mapper/")) {
                    // Resolve the symlink to get the actual dm-* device
                    std::error_code ec;
                    const auto real_path = fs::read_symlink(std::string{mount_device}, ec);
                    if (!ec) {
                        const auto dm_name = real_path.filename().string();
                        if (rng::find(dm_holders, dm_name) != dm_holders.end()) {
                            info.is_mounted = true;
                            info.mount_point = entry->mnt_dir;
                            info.filesystem = entry->mnt_type;
                            break;
                        }
                    }
                }
            }
        }
    }

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
