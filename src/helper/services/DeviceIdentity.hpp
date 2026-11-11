#pragma once

#include "algorithms/NvmeSanitize.hpp"
#include "models/WipePlan.hpp"
#include "util/Result.hpp"

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <filesystem>
#include <format>
#include <fstream>

namespace device_identity {

// A firmware command can reach beyond its namespace. Until shared subsystems
// are represented in the plan, accept only a fully visible local controller.
[[nodiscard]] inline auto validate_firmware_topology(const std::string& path,
                                                     const std::filesystem::path& sys_root = "/sys")
    -> std::expected<void, util::Error> {
    const auto controller = nvme_sanitize::controller_path_of(path);
    if (!controller) {
        return {};
    }
    namespace fs = std::filesystem;
    const auto rejected = [] {
        return std::unexpected(
            util::Error{"Cannot prove a single-controller, single-namespace NVMe topology; "
                        "shared or multipath firmware erases are unsupported"});
    };
    try {
        const auto controller_name = fs::path{*controller}.filename();
        const auto controller_path = sys_root / "class/nvme" / controller_name;
        const auto namespace_name = fs::path{path}.filename();
        if (fs::canonical(sys_root / "class/block" / namespace_name).parent_path() !=
            fs::canonical(controller_path)) {
            return rejected();
        }
        std::string nqn;
        std::ifstream nqn_file{controller_path / "subsysnqn"};
        if (!std::getline(nqn_file, nqn) || nqn.empty()) {
            return rejected();
        }
        unsigned int controllers = 0;
        for (const auto& entry : fs::directory_iterator{sys_root / "class/nvme"}) {
            std::string candidate;
            std::ifstream candidate_file{entry.path() / "subsysnqn"};
            if (!std::getline(candidate_file, candidate)) {
                return rejected();
            }
            controllers += candidate == nqn ? 1U : 0U;
        }
        unsigned int namespaces = 0;
        for (const auto& entry : fs::directory_iterator{controller_path}) {
            if (fs::exists(entry.path() / "nsid")) {
                ++namespaces;
            }
        }
        if (controllers != 1 || namespaces != 1) {
            return rejected();
        }
    } catch (const fs::filesystem_error&) {
        return rejected();
    }
    return {};
}

// All destructive work requires a kernel generation identifier. Serial numbers
// and device numbers alone can be reused after unplugging a drive.
[[nodiscard]] inline auto capture_kernel(int fd) -> std::expected<DeviceIdentity, util::Error> {
    struct stat status{};
    DeviceIdentity identity{};
    uint64_t size = 0;
    if (fstat(fd, &status) != 0 || !S_ISBLK(status.st_mode)) {
        return std::unexpected(util::Error{"Target is not an opened block device"});
    }
    identity.device_number = static_cast<uint64_t>(status.st_rdev);
    if (ioctl(fd, BLKGETDISKSEQ, &identity.disk_sequence) != 0 || identity.disk_sequence == 0) {
        return std::unexpected(
            util::Error{"Cannot establish device generation (BLKGETDISKSEQ); refusing to wipe"});
    }
    if (ioctl(fd, BLKGETSIZE64, &size) != 0 || size == 0) {
        return std::unexpected(util::Error{"Device size changed or could not be established"});
    }
    identity.disk.size_bytes = size;
    return identity;
}

[[nodiscard]] inline auto capture(int fd, const DiskInfo& disk)
    -> std::expected<DeviceIdentity, util::Error> {
    auto captured = capture_kernel(fd);
    if (!captured) {
        return std::unexpected(captured.error());
    }
    auto identity = std::move(*captured);
    if (identity.disk.size_bytes != disk.size_bytes) {
        return std::unexpected(util::Error{"Device size changed or could not be established"});
    }
    identity.disk = disk;
    if (disk.is_partition) {
        const auto device_number = static_cast<dev_t>(identity.device_number);
        const auto sys_path =
            std::format("/sys/dev/block/{}:{}/start", major(device_number), minor(device_number));
        std::ifstream start{sys_path};
        if (!(start >> identity.partition_start)) {
            return std::unexpected(util::Error{"Cannot establish partition start"});
        }
    }
    uint64_t sequence_after = 0;
    if (ioctl(fd, BLKGETDISKSEQ, &sequence_after) != 0 ||
        sequence_after != identity.disk_sequence) {
        return std::unexpected(util::Error{"Device changed while reading its identity"});
    }
    return identity;
}

[[nodiscard]] inline auto validate(int fd, const DeviceIdentity& expected)
    -> std::expected<void, util::Error> {
    const auto current = capture(fd, expected.disk);
    if (!current) {
        return std::unexpected(current.error());
    }
    if (current->device_number != expected.device_number ||
        current->disk_sequence != expected.disk_sequence ||
        current->partition_start != expected.partition_start) {
        return std::unexpected(util::Error{"Confirmed device identity changed; confirm again"});
    }
    return {};
}

}  // namespace device_identity
