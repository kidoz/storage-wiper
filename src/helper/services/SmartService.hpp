/**
 * @file SmartService.hpp
 * @brief SMART data retrieval service
 *
 * Reads SMART (Self-Monitoring, Analysis and Reporting Technology) data
 * from disk drives using Linux ATA/SCSI ioctls.
 */

#pragma once

#include "models/DiskInfo.hpp"

#include <string>

/**
 * @class SmartService
 * @brief Service for reading SMART data from storage devices
 *
 * Supports both ATA drives (SATA/IDE) and NVMe drives.
 * USB drives typically don't support SMART passthrough.
 */
class SmartService {
public:
    SmartService() = default;
    ~SmartService() = default;

    // Non-copyable
    SmartService(const SmartService&) = delete;
    SmartService& operator=(const SmartService&) = delete;
    SmartService(SmartService&&) = default;
    SmartService& operator=(SmartService&&) = default;

    /**
     * @brief Read SMART data for a device
     * @param device_path Device path (e.g., /dev/sda, /dev/nvme0n1)
     * @return SmartData with available information
     *
     * Returns SmartData with available=false if SMART is not supported
     * or cannot be read from the device.
     */
    [[nodiscard]] auto get_smart_data(const std::string& device_path) -> SmartData;

    /**
     * @brief Check if a device likely supports SMART
     * @param device_path Device path
     * @return true if device type typically supports SMART
     */
    [[nodiscard]] static auto is_smart_supported(const std::string& device_path) -> bool;

private:
    /**
     * @brief Read SMART data from an ATA device
     * @param device_path Device path
     * @return SmartData from ATA SMART
     */
    [[nodiscard]] auto read_ata_smart(const std::string& device_path) -> SmartData;

    /**
     * @brief Read SMART data from an NVMe device
     * @param device_path Device path
     * @return SmartData from NVMe health log
     */
    [[nodiscard]] auto read_nvme_smart(const std::string& device_path) -> SmartData;

    /**
     * @brief Calculate health status from SMART attributes
     * @param data SmartData with raw attributes
     * @return Derived health status
     */
    [[nodiscard]] static auto calculate_health_status(const SmartData& data)
        -> SmartData::HealthStatus;

    /**
     * @brief Parse ATA SMART attribute value
     * @param data Raw SMART data buffer
     * @param attr_id Attribute ID to find
     * @return Attribute raw value or -1 if not found
     */
    [[nodiscard]] static auto parse_ata_attribute(const uint8_t* data, uint8_t attr_id) -> int;
};
