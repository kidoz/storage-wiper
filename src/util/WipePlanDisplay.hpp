#pragma once

#include "models/WipePlan.hpp"

#include <string>

namespace util {

[[nodiscard]] inline auto wipe_method_label(const WipePlan& plan) -> std::string {
    const auto& method = plan.firmware.method;
    if (method == "overwrite")
        return "Software overwrite";
    if (method == "nvme-sanitize-crypto")
        return "NVMe Sanitize (cryptographic erase)";
    if (method == "nvme-sanitize-block")
        return "NVMe Sanitize (block erase)";
    if (method == "nvme-sanitize-overwrite")
        return "NVMe Sanitize (overwrite)";
    if (method == "nvme-format-crypto")
        return "NVMe Format NVM (cryptographic erase)";
    if (method == "ata-enhanced-security-erase")
        return "ATA Enhanced Security Erase";
    if (method == "ata-security-erase")
        return "ATA Security Erase";
    return method;
}

[[nodiscard]] inline auto wipe_scope_label(const WipePlan& plan) -> std::string {
    if (plan.firmware.scope == "nvme-subsystem") {
        return "All namespaces in the NVMe subsystem, including their partitions and partition "
               "tables";
    }
    if (plan.firmware.scope == "device") {
        if (plan.device.disk.is_partition) {
            return "Only the selected partition; other partitions and the parent partition table "
                   "are preserved";
        }
        return "Only the selected device, including its partitions and partition table";
    }
    return plan.firmware.scope;
}

}  // namespace util
