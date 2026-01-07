#pragma once

#include "services/IDiskService.hpp"

#include <algorithm>

namespace device_policy {

inline auto validate_wipe_target(IDiskService& disk_service, const std::string& path)
    -> std::expected<void, util::Error> {
    if (path.empty()) {
        return std::unexpected(util::Error{"Device path is empty"});
    }

    if (auto valid = disk_service.validate_device_path(path); !valid) {
        return std::unexpected(valid.error());
    }

    auto disks = disk_service.get_available_disks();
    auto it = std::find_if(disks.begin(), disks.end(),
                           [&path](const DiskInfo& disk) { return disk.path == path; });
    if (it == disks.end()) {
        return std::unexpected(util::Error{"Device not found"});
    }

    if (it->is_mounted) {
        return std::unexpected(util::Error{"Device is mounted. Unmount before wiping."});
    }

    if (!disk_service.is_disk_writable(path)) {
        return std::unexpected(util::Error{"Device is not writable"});
    }

    return {};
}

} // namespace device_policy
