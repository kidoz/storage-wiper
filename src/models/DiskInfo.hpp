/**
 * @file DiskInfo.hpp
 * @brief Data model for storage device information
 */

#pragma once

#include <cstdint>
#include <string>

/**
 * @struct SmartData
 * @brief SMART (Self-Monitoring, Analysis and Reporting Technology) data for a disk
 */
struct SmartData {
    /**
     * @enum HealthStatus
     * @brief Overall disk health status derived from SMART attributes
     */
    enum class HealthStatus {
        UNKNOWN,   ///< SMART not available or couldn't be read
        GOOD,      ///< All attributes within normal ranges
        WARNING,   ///< Some attributes showing potential issues
        CRITICAL   ///< Imminent failure indicators present
    };

    bool available = false;           ///< Whether SMART data was successfully retrieved
    bool healthy = true;              ///< Overall health assessment (true = PASSED)
    int64_t power_on_hours = -1;      ///< Total power-on hours (-1 if unknown)
    int reallocated_sectors = -1;     ///< Count of reallocated sectors (-1 if unknown)
    int pending_sectors = -1;         ///< Current pending sector count (-1 if unknown)
    int temperature_celsius = -1;     ///< Current temperature in Celsius (-1 if unknown)
    int uncorrectable_errors = -1;    ///< Uncorrectable error count (-1 if unknown)
    HealthStatus status = HealthStatus::UNKNOWN;  ///< Derived health status

    auto operator==(const SmartData&) const -> bool = default;

    /**
     * @brief Get human-readable status string
     * @return Status description
     */
    [[nodiscard]] auto status_string() const -> std::string {
        switch (status) {
            case HealthStatus::UNKNOWN:
                return "Unknown";
            case HealthStatus::GOOD:
                return "Good";
            case HealthStatus::WARNING:
                return "Warning";
            case HealthStatus::CRITICAL:
                return "Critical";
        }
        return "Unknown";
    }
};

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
    SmartData smart;            ///< SMART health data

    auto operator==(const DiskInfo&) const -> bool = default;
};
