/**
 * @file SmartService.hpp
 * @brief SMART data retrieval service
 *
 * Reads SMART (Self-Monitoring, Analysis and Reporting Technology) data
 * from disk drives using Linux ATA/SCSI/NVMe ioctls.
 */

#pragma once

#include "models/DiskInfo.hpp"

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @class SmartService
 * @brief Service for reading SMART data from storage devices
 *
 * Supports:
 * - ATA/SATA drives (HDD and SSD) via HDIO_DRIVE_CMD, falling back to
 *   SCSI/ATA translation (SAT) pass-through over SG_IO for USB bridges,
 *   SAS controllers and other SCSI-attached devices
 * - NVMe drives via the SMART/Health Information log page
 * - eMMC devices via the EXT_CSD life-time registers exposed in sysfs
 *
 * SD cards and virtual devices report no health data.
 */
class SmartService {
public:
    /// Size of the ATA SMART READ DATA structure in bytes
    static constexpr size_t SMART_DATA_SIZE = 512;

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
     *
     * Blocking, and potentially for a long time: a sleeping drive has to spin
     * up before it answers. Call it off any latency-sensitive path.
     */
    [[nodiscard]] auto get_smart_data(const std::string& device_path) -> SmartData;

    /**
     * @brief Check if a device likely supports SMART
     * @param device_path Device path
     * @return true if device type typically supports SMART
     */
    [[nodiscard]] static auto is_smart_supported(const std::string& device_path) -> bool;

    // --- Pure helpers, public so they can be unit tested without hardware ---

    /**
     * @brief Verify the checksum of an ATA SMART READ DATA structure
     * @param data 512-byte SMART data buffer
     * @return true if the byte sum is zero modulo 256, as required by ATA
     */
    [[nodiscard]] static auto is_checksum_valid(const uint8_t* data) -> bool;

    /**
     * @brief Check whether a SMART data structure contains any attribute entries
     * @param data 512-byte SMART data buffer
     * @return true if at least one attribute slot has a non-zero attribute ID
     *
     * Guards against reporting an all-zero buffer as valid health data.
     */
    [[nodiscard]] static auto has_attributes(const uint8_t* data) -> bool;

    /**
     * @brief Read the raw value of a SMART attribute
     * @param data 512-byte SMART data buffer
     * @param attr_id Attribute ID to find
     * @return Lower 32 bits of the raw value, or -1 if the attribute is absent
     */
    [[nodiscard]] static auto parse_ata_attribute(const uint8_t* data, uint8_t attr_id) -> int64_t;

    /**
     * @brief Read the normalised current value of a SMART attribute
     * @param data 512-byte SMART data buffer
     * @param attr_id Attribute ID to find
     * @return Normalised value (typically 0-253), or -1 if the attribute is absent
     */
    [[nodiscard]] static auto parse_ata_attribute_value(const uint8_t* data, uint8_t attr_id)
        -> int;

    /**
     * @brief Populate a SmartData record from a raw ATA SMART data structure
     * @param data 512-byte SMART data buffer
     * @param rotational Whether the device is a spinning disk. SSD wear
     *        attributes are only interpreted for non-rotational devices.
     * @param result Record to populate
     */
    static void parse_ata_smart_data(const uint8_t* data, bool rotational, SmartData& result);

    /**
     * @brief Temperature bands, in Celsius, used to classify a device's health
     *
     * The bands differ by device class: an NVMe module idles near 50 C where a
     * hard drive or a 2.5" SATA SSD is already hot, so one set of numbers cannot
     * serve both. The bands are advisory thresholds in the client; they do not
     * replace the device's own verdict, which is reported separately (an NVMe
     * critical warning bit, or an ATA threshold-exceeded status).
     */
    struct TemperatureLimits {
        int warning;   ///< At or above this reading the health is WARNING
        int critical;  ///< At or above this reading the health is CRITICAL

        auto operator==(const TemperatureLimits&) const -> bool = default;
    };

    /// Bands applied to ATA (HDD and SATA SSD) and eMMC devices
    static constexpr TemperatureLimits ATA_TEMPERATURE_LIMITS{50, 60};

    /// Bands applied to NVMe devices, which run hotter by design
    static constexpr TemperatureLimits NVME_TEMPERATURE_LIMITS{70, 80};

    /**
     * @brief Temperature bands that apply to a device path
     * @param device_path Device path (e.g., /dev/sda, /dev/nvme0n1)
     * @return Bands for the device class the path names
     */
    [[nodiscard]] static auto temperature_limits_for(std::string_view device_path)
        -> TemperatureLimits;

    /**
     * @brief Calculate health status from SMART attributes
     * @param data SmartData with raw attributes
     * @param limits Temperature bands for the device class, ATA bands by default
     * @return Derived health status
     */
    [[nodiscard]] static auto calculate_health_status(
        const SmartData& data, TemperatureLimits limits = ATA_TEMPERATURE_LIMITS)
        -> SmartData::HealthStatus;

private:
    /**
     * @brief Read SMART data from an ATA device (SATA/IDE, HDD or SSD)
     * @param device_path Device path
     * @return SmartData from ATA SMART
     */
    [[nodiscard]] auto read_ata_smart(const std::string& device_path) -> SmartData;

    /**
     * @brief Read SMART data from an NVMe device
     * @param device_path Device path
     * @return SmartData from the NVMe SMART/Health log page
     */
    [[nodiscard]] auto read_nvme_smart(const std::string& device_path) -> SmartData;

    /**
     * @brief Read health data for an eMMC device from sysfs
     * @param device_path Device path
     * @return SmartData derived from EXT_CSD life-time and pre-EOL registers
     */
    [[nodiscard]] auto read_mmc_health(const std::string& device_path) -> SmartData;
};
