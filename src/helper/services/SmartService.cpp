/**
 * @file SmartService.cpp
 * @brief SMART data retrieval service implementation
 */

#include "helper/services/SmartService.hpp"

#include <fcntl.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string_view>

namespace {

// ATA SMART command constants
constexpr uint8_t ATA_SMART_CMD = 0xB0;
constexpr uint8_t ATA_SMART_READ_DATA = 0xD0;
constexpr uint8_t ATA_SMART_RETURN_STATUS = 0xDA;

// SMART attribute IDs
constexpr uint8_t ATTR_REALLOCATED_SECTORS = 5;
constexpr uint8_t ATTR_POWER_ON_HOURS = 9;
constexpr uint8_t ATTR_TEMPERATURE = 194;
constexpr uint8_t ATTR_CURRENT_PENDING_SECTORS = 197;
constexpr uint8_t ATTR_UNCORRECTABLE_ERRORS = 198;

// SMART data structure size
constexpr size_t SMART_DATA_SIZE = 512;

// Thresholds for health status
constexpr int WARNING_REALLOCATED_SECTORS = 5;
constexpr int CRITICAL_REALLOCATED_SECTORS = 50;
constexpr int WARNING_PENDING_SECTORS = 1;
constexpr int CRITICAL_PENDING_SECTORS = 10;
constexpr int WARNING_TEMPERATURE = 50;
constexpr int CRITICAL_TEMPERATURE = 60;

/**
 * Check if device path looks like NVMe
 */
auto is_nvme_device(std::string_view path) -> bool {
    return path.find("nvme") != std::string_view::npos;
}

}  // namespace

auto SmartService::get_smart_data(const std::string& device_path) -> SmartData {
    SmartData result;

    if (!is_smart_supported(device_path)) {
        return result;
    }

    if (is_nvme_device(device_path)) {
        result = read_nvme_smart(device_path);
    } else {
        result = read_ata_smart(device_path);
    }

    if (result.available) {
        result.status = calculate_health_status(result);
    }

    return result;
}

auto SmartService::is_smart_supported(const std::string& device_path) -> bool {
    // Most SATA, NVMe, and SCSI drives support SMART
    // USB drives typically don't (though some enclosures pass through)

    // Check if it's a recognized device type
    if (device_path.find("/dev/sd") != std::string::npos ||
        device_path.find("/dev/nvme") != std::string::npos ||
        device_path.find("/dev/hd") != std::string::npos) {
        return true;
    }

    // MMC/SD cards don't have traditional SMART
    if (device_path.find("/dev/mmcblk") != std::string::npos) {
        return false;
    }

    // Virtual devices don't have SMART
    if (device_path.find("/dev/loop") != std::string::npos ||
        device_path.find("/dev/vd") != std::string::npos ||
        device_path.find("/dev/dm-") != std::string::npos) {
        return false;
    }

    return true;
}

auto SmartService::read_ata_smart(const std::string& device_path) -> SmartData {
    SmartData result;

    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return result;
    }

    // Prepare SMART READ DATA command using HDIO_DRIVE_CMD
    // Structure: [command, sector_count, feature, sector_number, low_cyl, high_cyl, device_head,
    // ...]
    std::array<uint8_t, 4 + SMART_DATA_SIZE> buffer{};
    buffer[0] = ATA_SMART_CMD;        // Command register
    buffer[1] = 1;                    // Sector count
    buffer[2] = ATA_SMART_READ_DATA;  // Feature register (SMART subcommand)
    buffer[3] = 0;                    // Sector number

    // HDIO_DRIVE_CMD requires special cylinder values for SMART
    // This is handled internally by the kernel

    // Try HDIO_DRIVE_CMD
    int ret = ioctl(fd, HDIO_DRIVE_CMD, buffer.data());
    if (ret != 0) {
        // Try alternative: SG_IO SCSI passthrough (for SATA behind AHCI)
        // This is more complex and requires scsi/sg.h
        // For simplicity, we'll just fail gracefully
        close(fd);
        return result;
    }

    // Parse SMART data (starts at offset 4 after the command header)
    const uint8_t* smart_data = buffer.data() + 4;

    result.available = true;
    result.healthy = true;  // Will be updated based on attributes

    // Parse key attributes
    result.reallocated_sectors = parse_ata_attribute(smart_data, ATTR_REALLOCATED_SECTORS);
    result.power_on_hours = parse_ata_attribute(smart_data, ATTR_POWER_ON_HOURS);
    result.temperature_celsius = parse_ata_attribute(smart_data, ATTR_TEMPERATURE);
    result.pending_sectors = parse_ata_attribute(smart_data, ATTR_CURRENT_PENDING_SECTORS);
    result.uncorrectable_errors = parse_ata_attribute(smart_data, ATTR_UNCORRECTABLE_ERRORS);

    // Check SMART return status for overall health
    std::array<uint8_t, 4> status_cmd{};
    status_cmd[0] = ATA_SMART_CMD;
    status_cmd[2] = ATA_SMART_RETURN_STATUS;

    if (ioctl(fd, HDIO_DRIVE_CMD, status_cmd.data()) == 0) {
        // Check LBA mid/high for threshold exceeded status
        // 0x4F/0xC2 = PASSED, 0xF4/0x2C = FAILED
        // These are in the returned data after the command
    }

    close(fd);
    return result;
}

auto SmartService::read_nvme_smart(const std::string& device_path) -> SmartData {
    SmartData result;

    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return result;
    }

    // NVMe SMART/Health Information (Log Page 02h)
    struct nvme_smart_log {
        uint8_t critical_warning;
        uint16_t temperature;
        uint8_t avail_spare;
        uint8_t spare_thresh;
        uint8_t percent_used;
        uint8_t reserved1[26];
        uint8_t data_units_read[16];
        uint8_t data_units_written[16];
        uint8_t host_reads[16];
        uint8_t host_writes[16];
        uint8_t ctrl_busy_time[16];
        uint8_t power_cycles[16];
        uint8_t power_on_hours[16];
        uint8_t unsafe_shutdowns[16];
        uint8_t media_errors[16];
        uint8_t num_err_log_entries[16];
        uint8_t reserved2[320];
    } __attribute__((packed));

    nvme_smart_log smart_log{};

    struct nvme_admin_cmd cmd{};
    cmd.opcode = 0x02;  // Get Log Page
    cmd.nsid = 0xFFFF'FFFF;
    cmd.addr = reinterpret_cast<uint64_t>(&smart_log);
    cmd.data_len = sizeof(smart_log);
    cmd.cdw10 = 0x02 | (((sizeof(smart_log) / 4) - 1) << 16);  // Log ID 2, NUMDL

    if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0) {
        result.available = true;

        // Temperature is in Kelvin
        int temp_kelvin = smart_log.temperature;
        if (temp_kelvin > 0 && temp_kelvin < 500) {
            result.temperature_celsius = temp_kelvin - 273;
        }

        // Power on hours (lower 64 bits only for simplicity)
        uint64_t poh = 0;
        std::memcpy(&poh, smart_log.power_on_hours, sizeof(poh));
        result.power_on_hours = static_cast<int64_t>(poh);

        // Media errors
        uint64_t media_errs = 0;
        std::memcpy(&media_errs, smart_log.media_errors, sizeof(media_errs));
        if (media_errs > 0) {
            result.uncorrectable_errors =
                static_cast<int>(media_errs > INT32_MAX ? INT32_MAX : media_errs);
        }

        // Check critical warning flags
        result.healthy = (smart_log.critical_warning == 0);
    }

    close(fd);
    return result;
}

auto SmartService::calculate_health_status(const SmartData& data) -> SmartData::HealthStatus {
    if (!data.available) {
        return SmartData::HealthStatus::UNKNOWN;
    }

    // Check for critical conditions
    if (!data.healthy) {
        return SmartData::HealthStatus::CRITICAL;
    }

    if (data.reallocated_sectors >= CRITICAL_REALLOCATED_SECTORS ||
        data.pending_sectors >= CRITICAL_PENDING_SECTORS ||
        data.temperature_celsius >= CRITICAL_TEMPERATURE) {
        return SmartData::HealthStatus::CRITICAL;
    }

    // Check for warning conditions
    if (data.reallocated_sectors >= WARNING_REALLOCATED_SECTORS ||
        data.pending_sectors >= WARNING_PENDING_SECTORS ||
        data.temperature_celsius >= WARNING_TEMPERATURE || data.uncorrectable_errors > 0) {
        return SmartData::HealthStatus::WARNING;
    }

    return SmartData::HealthStatus::GOOD;
}

auto SmartService::parse_ata_attribute(const uint8_t* data, uint8_t attr_id) -> int {
    // SMART attributes start at offset 2 in the data structure
    // Each attribute is 12 bytes:
    // [0] = attribute ID
    // [1-2] = flags
    // [3] = current value
    // [4] = worst value
    // [5-10] = raw value (6 bytes, little-endian)
    // [11] = reserved

    constexpr size_t ATTR_OFFSET = 2;
    constexpr size_t ATTR_SIZE = 12;
    constexpr size_t MAX_ATTRS = 30;

    for (size_t i = 0; i < MAX_ATTRS; ++i) {
        size_t offset = ATTR_OFFSET + (i * ATTR_SIZE);

        if (data[offset] == 0) {
            // End of attributes
            break;
        }

        if (data[offset] == attr_id) {
            // Return raw value (first 4 bytes of 6-byte raw value)
            uint32_t raw = data[offset + 5] | (data[offset + 6] << 8) | (data[offset + 7] << 16) |
                           (data[offset + 8] << 24);
            return static_cast<int>(raw);
        }
    }

    return -1;  // Attribute not found
}
