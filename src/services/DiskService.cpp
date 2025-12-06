#include "services/DiskService.hpp"
#include "util/FileDescriptor.hpp"

// Standard library
#include <algorithm>
#include <array>
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
            return name.find(pattern) != std::string_view::npos;
        });
    };
    
    std::vector<DiskInfo> disks;
    
    for (const auto& entry : fs::directory_iterator{block_dir}) {
        const auto device_name = entry.path().filename().string();
        
        if (is_virtual_device(device_name)) {
            continue;
        }
        
        const auto device_path = std::format("/dev/{}", device_name);
        
        if (!validate_device_path(device_path)) {
            continue;
        }
        
        if (auto info = parse_disk_info(device_path); info.size_bytes > 0) {
            disks.emplace_back(std::move(info));
        }
    }
    
    return disks;
}

auto DiskService::unmount_disk(const std::string& path) -> bool {
    if (!validate_device_path(path)) {
        return false;
    }
    
    // Try force unmount first, then lazy unmount as fallback
    constexpr std::array unmount_flags{MNT_FORCE, MNT_DETACH};
    
    return rng::any_of(unmount_flags, [&path](int flags) noexcept {
        return ::umount2(path.c_str(), flags) == 0;
    });
}

auto DiskService::is_disk_writable(const std::string& path) -> bool {
    if (!validate_device_path(path)) {
        return false;
    }

    const util::FileDescriptor fd{::open(path.c_str(), O_RDWR)};
    return fd.is_valid();
}

auto DiskService::get_disk_size(const std::string& path) -> uint64_t {
    if (!validate_device_path(path)) {
        return 0;
    }

    const util::FileDescriptor fd{::open(path.c_str(), O_RDONLY)};
    if (!fd) {
        return 0;
    }

    uint64_t size{};
    return (::ioctl(fd.get(), BLKGETSIZE64, &size) == 0) ? size : 0;
}

auto DiskService::validate_device_path(const std::string& path) -> bool {
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
        return false;
    }

    // Verify it's actually a block device
    struct stat st{};
    return (::stat(path.c_str(), &st) == 0) && S_ISBLK(st.st_mode);
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
        .mount_point = {}
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
    
    // Check mount status using RAII wrapper
    if (auto mtab_deleter = [](FILE* f) { if (f) ::endmntent(f); };
        std::unique_ptr<FILE, decltype(mtab_deleter)> mtab{::setmntent("/proc/mounts", "r"), mtab_deleter}) {
        
        while (auto* entry = ::getmntent(mtab.get())) {
            const std::string_view mount_device{entry->mnt_fsname};
            
            if (mount_device == device_path || mount_device.find(device_name) != std::string_view::npos) {
                info.is_mounted = true;
                info.mount_point = entry->mnt_dir;
                info.filesystem = entry->mnt_type;
                break;
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