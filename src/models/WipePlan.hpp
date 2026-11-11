#pragma once

#include "models/DiskInfo.hpp"
#include "models/WipeTypes.hpp"

#include <string>
#include <vector>

// Kernel disk sequence numbers distinguish devices even when their /dev name,
// major/minor numbers and advertised serial number are reused.
struct DeviceIdentity {
    DiskInfo disk;
    uint64_t device_number = 0;
    uint64_t disk_sequence = 0;
    uint64_t partition_start = 0;

    auto operator==(const DeviceIdentity&) const -> bool = default;
};

struct FirmwarePlan {
    std::string method;
    std::string scope;
    bool cancellable = false;

    auto operator==(const FirmwarePlan&) const -> bool = default;
};

struct WipePlan {
    static constexpr uint32_t VERSION = 2;
    uint32_t version = VERSION;
    std::string id;
    DeviceIdentity device;
    WipeAlgorithm algorithm = WipeAlgorithm::ZERO_FILL;
    bool verify = false;
    FirmwarePlan firmware;
    std::vector<DeviceIdentity> targets;
};
