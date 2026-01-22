/**
 * @file DiskInfo.hpp
 * @brief Data model for storage device information
 */

#pragma once

#include <cstdint>
#include <string>

/**
 * @struct DiskInfo
 * @brief Contains information about a storage device
 */
struct DiskInfo {
    std::string path;           ///< Device path (e.g., /dev/sda)
    std::string model;          ///< Device model name
    std::string serial;         ///< Device serial number
    uint64_t size_bytes = 0;    ///< Size in bytes
    bool is_removable = false;  ///< Whether device is removable
    bool is_ssd = false;        ///< Whether device is an SSD
    std::string filesystem;     ///< Filesystem type if mounted
    bool is_mounted = false;    ///< Mount status (direct or via LVM/dm)
    std::string mount_point;    ///< Mount point path
    bool is_lvm_pv = false;     ///< Whether device is an LVM Physical Volume or has dm holders

    auto operator==(const DiskInfo&) const -> bool = default;
};
