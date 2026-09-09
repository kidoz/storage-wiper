#pragma once

#include "algorithms/NvmeSanitize.hpp"
#include "helper/services/DevicePathMatcher.hpp"
#include "models/WipeTypes.hpp"
#include "services/IDiskService.hpp"

#include <algorithm>

namespace device_policy {

inline auto resolve_wipe_targets(IDiskService& disk_service, const std::string& path,
                                 WipeAlgorithm algorithm)
    -> std::expected<std::vector<std::string>, util::Error> {
    if (path.empty()) {
        return std::unexpected(util::Error{"Device path is empty"});
    }

    if (auto valid = disk_service.validate_device_path(path); !valid) {
        return std::unexpected(valid.error());
    }

    // Bypass cache to get fresh mount status for validation
    disk_service.invalidate_cache();
    auto disks_res = disk_service.get_available_disks_blocking();
    if (!disks_res) {
        return std::unexpected(disks_res.error());
    }
    const auto& disks = *disks_res;

    auto it = std::find_if(disks.begin(), disks.end(),
                           [&path](const DiskInfo& disk) { return disk.path == path; });
    if (it == disks.end()) {
        return std::unexpected(util::Error{"Device not found"});
    }

    const bool hardware = algorithm == WipeAlgorithm::ATA_SECURE_ERASE;
    if (hardware && it->is_partition) {
        return std::unexpected(util::Error{"Hardware secure erase cannot target a partition. "
                                           "Select a whole disk or use an overwrite algorithm."});
    }

    std::vector<std::string> targets{path};
    const auto controller = nvme_sanitize::controller_path_of(path);
    if (hardware && controller) {
        for (const auto& disk : disks) {
            if (!disk.is_partition && disk.path != path &&
                nvme_sanitize::controller_path_of(disk.path) == controller) {
                targets.push_back(disk.path);
            }
        }
    }

    for (const auto& target : targets) {
        for (const auto& disk : disks) {
            if (device_path_matcher::is_device_or_partition_of(target, disk.path) &&
                disk.is_mounted) {
                return std::unexpected(
                    util::Error{"Device is mounted: " + disk.path + ". Unmount before wiping."});
            }
        }
        if (auto valid = disk_service.validate_device_path(target); !valid) {
            return std::unexpected(valid.error());
        }
        if (!disk_service.is_disk_writable(target)) {
            return std::unexpected(util::Error{"Device is not writable: " + target});
        }
    }
    return targets;
}

}  // namespace device_policy
