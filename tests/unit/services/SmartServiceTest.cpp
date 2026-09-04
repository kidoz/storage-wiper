/**
 * @file SmartServiceTest.cpp
 * @brief Unit tests for SMART attribute parsing and health classification
 *
 * These tests exercise the pure parts of SmartService: the attribute table
 * decoder and the health status rules. The ioctl transports need real
 * hardware and privileges, so they are not covered here.
 */

#include "helper/services/SmartService.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using Buffer = std::array<uint8_t, SmartService::SMART_DATA_SIZE>;

constexpr size_t ATTR_TABLE_OFFSET = 2;
constexpr size_t ATTR_ENTRY_SIZE = 12;

/**
 * Write one attribute entry into a SMART data buffer
 *
 * @param buffer Buffer to modify
 * @param slot Attribute slot index (0-29)
 * @param attr_id Attribute ID
 * @param normalized Normalised current value
 * @param raw 48-bit raw value
 */
void set_attribute(Buffer& buffer, size_t slot, uint8_t attr_id, uint8_t normalized,
                   uint64_t raw = 0) {
    const size_t offset = ATTR_TABLE_OFFSET + (slot * ATTR_ENTRY_SIZE);
    buffer[offset] = attr_id;
    buffer[offset + 1] = 0x00;  // Flags
    buffer[offset + 2] = 0x00;
    buffer[offset + 3] = normalized;
    buffer[offset + 4] = normalized;  // Worst
    for (size_t i = 0; i < 6; ++i) {
        buffer[offset + 5 + i] = static_cast<uint8_t>((raw >> (i * 8)) & 0xFF);
    }
}

/**
 * Set the trailing checksum byte so the structure sums to zero modulo 256
 */
void fix_checksum(Buffer& buffer) {
    buffer[SmartService::SMART_DATA_SIZE - 1] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < SmartService::SMART_DATA_SIZE - 1; ++i) {
        sum = static_cast<uint8_t>(sum + buffer[i]);
    }
    buffer[SmartService::SMART_DATA_SIZE - 1] = static_cast<uint8_t>(0x100 - sum);
}

/**
 * Build a plausible HDD SMART structure
 */
auto make_hdd_buffer() -> Buffer {
    Buffer buffer{};
    buffer[0] = 0x10;                                        // Revision number
    set_attribute(buffer, 0, 5, 100, 0);                     // Reallocated sectors
    set_attribute(buffer, 1, 9, 95, 12'345);                 // Power-on hours
    set_attribute(buffer, 2, 194, 65, 0x000F'0018'0023ULL);  // Temperature 0x23 = 35C
    set_attribute(buffer, 3, 197, 100, 0);                   // Pending sectors
    set_attribute(buffer, 4, 198, 100, 0);                   // Uncorrectable errors
    fix_checksum(buffer);
    return buffer;
}

}  // namespace

// === Checksum and structure validation ===

TEST(SmartServiceTest, ChecksumValidForWellFormedStructure) {
    auto buffer = make_hdd_buffer();
    EXPECT_TRUE(SmartService::is_checksum_valid(buffer.data()));
}

TEST(SmartServiceTest, ChecksumInvalidWhenByteIsCorrupted) {
    auto buffer = make_hdd_buffer();
    buffer[100] = static_cast<uint8_t>(buffer[100] + 1);
    EXPECT_FALSE(SmartService::is_checksum_valid(buffer.data()));
}

TEST(SmartServiceTest, AllZeroBufferHasNoAttributes) {
    Buffer buffer{};
    // An all-zero buffer passes the checksum test but carries no data. This is
    // exactly what a failed ioctl used to leave behind, and it must not be
    // reported as a healthy drive.
    EXPECT_TRUE(SmartService::is_checksum_valid(buffer.data()));
    EXPECT_FALSE(SmartService::has_attributes(buffer.data()));
}

TEST(SmartServiceTest, PopulatedBufferHasAttributes) {
    auto buffer = make_hdd_buffer();
    EXPECT_TRUE(SmartService::has_attributes(buffer.data()));
}

// === Attribute table decoding ===

TEST(SmartServiceTest, ParsesRawAttributeValue) {
    auto buffer = make_hdd_buffer();
    EXPECT_EQ(SmartService::parse_ata_attribute(buffer.data(), 9), 12'345);
}

TEST(SmartServiceTest, ParsesNormalizedAttributeValue) {
    auto buffer = make_hdd_buffer();
    EXPECT_EQ(SmartService::parse_ata_attribute_value(buffer.data(), 9), 95);
}

TEST(SmartServiceTest, MissingAttributeReturnsNegativeOne) {
    auto buffer = make_hdd_buffer();
    EXPECT_EQ(SmartService::parse_ata_attribute(buffer.data(), 177), -1);
    EXPECT_EQ(SmartService::parse_ata_attribute_value(buffer.data(), 177), -1);
}

TEST(SmartServiceTest, SkipsEmptySlotsInAttributeTable) {
    Buffer buffer{};
    buffer[0] = 0x10;
    // Leave slots 0-4 empty; the scan must not stop at the first zero ID
    set_attribute(buffer, 5, 9, 90, 4'242);
    fix_checksum(buffer);

    EXPECT_EQ(SmartService::parse_ata_attribute(buffer.data(), 9), 4'242);
}

TEST(SmartServiceTest, ReadsLastAttributeSlot) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 29, 5, 100, 7);
    fix_checksum(buffer);

    EXPECT_EQ(SmartService::parse_ata_attribute(buffer.data(), 5), 7);
}

TEST(SmartServiceTest, AttributeIdZeroIsNeverMatched) {
    Buffer buffer{};
    EXPECT_EQ(SmartService::parse_ata_attribute(buffer.data(), 0), -1);
}

// === HDD interpretation ===

TEST(SmartServiceTest, ParsesHddAttributesIntoRecord) {
    auto buffer = make_hdd_buffer();
    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), true, data);

    EXPECT_EQ(data.power_on_hours, 12'345);
    EXPECT_EQ(data.reallocated_sectors, 0);
    EXPECT_EQ(data.pending_sectors, 0);
    EXPECT_EQ(data.uncorrectable_errors, 0);
}

TEST(SmartServiceTest, TemperatureIgnoresVendorMinMaxBytes) {
    auto buffer = make_hdd_buffer();
    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), true, data);

    // Raw value packs min/max readings above the current temperature; only the
    // low byte is the reading itself.
    EXPECT_EQ(data.temperature_celsius, 35);
}

TEST(SmartServiceTest, FallsBackToAirflowTemperature) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 190, 60, 42);  // Airflow temperature, no attribute 194
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), true, data);
    EXPECT_EQ(data.temperature_celsius, 42);
}

TEST(SmartServiceTest, ImplausibleTemperatureIsReportedAsUnknown) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 194, 60, 0xFF);  // 255 C is not a real reading
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), true, data);
    EXPECT_EQ(data.temperature_celsius, -1);
}

TEST(SmartServiceTest, RotationalDeviceReportsNoWearLevel) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 233, 70, 0);  // Wear attribute present but device is an HDD
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), true, data);
    EXPECT_EQ(data.percentage_used, -1);
}

// === SSD interpretation ===

TEST(SmartServiceTest, DerivesWearFromMediaWearoutIndicator) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 233, 70, 0);  // 70% life left
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), false, data);
    EXPECT_EQ(data.percentage_used, 30);
}

TEST(SmartServiceTest, FallsBackToWearLevelingCount) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 177, 88, 0);
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), false, data);
    EXPECT_EQ(data.percentage_used, 12);
}

TEST(SmartServiceTest, PrefersMediaWearoutOverLifeLeftAttribute) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 231, 40, 0);  // Reused as temperature by some controllers
    set_attribute(buffer, 1, 233, 90, 0);
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), false, data);
    EXPECT_EQ(data.percentage_used, 10);
}

TEST(SmartServiceTest, IgnoresOutOfRangeWearValue) {
    Buffer buffer{};
    buffer[0] = 0x10;
    set_attribute(buffer, 0, 233, 253, 0);  // Not a percentage
    fix_checksum(buffer);

    SmartData data;
    SmartService::parse_ata_smart_data(buffer.data(), false, data);
    EXPECT_EQ(data.percentage_used, -1);
}

// === Health classification ===

TEST(SmartServiceTest, UnavailableDataIsUnknown) {
    SmartData data;
    data.available = false;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::UNKNOWN);
}

TEST(SmartServiceTest, CleanDriveIsGood) {
    SmartData data;
    data.available = true;
    data.reallocated_sectors = 0;
    data.pending_sectors = 0;
    data.uncorrectable_errors = 0;
    data.temperature_celsius = 35;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::GOOD);
}

TEST(SmartServiceTest, DriveFailureVerdictIsCritical) {
    SmartData data;
    data.available = true;
    data.healthy = false;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::CRITICAL);
}

TEST(SmartServiceTest, ReallocatedSectorsRaiseWarningThenCritical) {
    SmartData data;
    data.available = true;

    data.reallocated_sectors = 5;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::WARNING);

    data.reallocated_sectors = 50;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::CRITICAL);
}

TEST(SmartServiceTest, PendingSectorsRaiseWarning) {
    SmartData data;
    data.available = true;
    data.pending_sectors = 1;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::WARNING);
}

TEST(SmartServiceTest, HighTemperatureRaisesWarningThenCritical) {
    SmartData data;
    data.available = true;

    data.temperature_celsius = 50;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::WARNING);

    data.temperature_celsius = 60;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::CRITICAL);
}

TEST(SmartServiceTest, WearLevelRaisesWarningThenCritical) {
    SmartData data;
    data.available = true;

    data.percentage_used = 90;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::WARNING);

    data.percentage_used = 100;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::CRITICAL);
}

TEST(SmartServiceTest, SpareBelowThresholdIsCritical) {
    SmartData data;
    data.available = true;
    data.available_spare_percent = 5;
    data.available_spare_threshold_percent = 10;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::CRITICAL);
}

TEST(SmartServiceTest, SpareNearThresholdIsWarning) {
    SmartData data;
    data.available = true;
    data.available_spare_percent = 15;
    data.available_spare_threshold_percent = 10;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::WARNING);
}

TEST(SmartServiceTest, HealthySpareIsGood) {
    SmartData data;
    data.available = true;
    data.available_spare_percent = 100;
    data.available_spare_threshold_percent = 10;
    data.percentage_used = 3;
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::GOOD);
}

TEST(SmartServiceTest, UnknownAttributesDoNotTriggerWarnings) {
    SmartData data;
    data.available = true;
    // Every attribute unknown (-1) must stay GOOD rather than tripping a threshold
    EXPECT_EQ(SmartService::calculate_health_status(data), SmartData::HealthStatus::GOOD);
}

// === Device support matrix ===

TEST(SmartServiceTest, SupportsAtaNvmeAndMmcDevices) {
    EXPECT_TRUE(SmartService::is_smart_supported("/dev/sda"));
    EXPECT_TRUE(SmartService::is_smart_supported("/dev/nvme0n1"));
    EXPECT_TRUE(SmartService::is_smart_supported("/dev/hda"));
    EXPECT_TRUE(SmartService::is_smart_supported("/dev/mmcblk0"));
}

TEST(SmartServiceTest, RejectsVirtualDevices) {
    EXPECT_FALSE(SmartService::is_smart_supported("/dev/loop0"));
    EXPECT_FALSE(SmartService::is_smart_supported("/dev/vda"));
    EXPECT_FALSE(SmartService::is_smart_supported("/dev/dm-0"));
    EXPECT_FALSE(SmartService::is_smart_supported("/dev/zram0"));
    EXPECT_FALSE(SmartService::is_smart_supported("/dev/ram0"));
}

TEST(SmartServiceTest, RejectsUnknownDevicePaths) {
    EXPECT_FALSE(SmartService::is_smart_supported("/dev/sr0"));
    EXPECT_FALSE(SmartService::is_smart_supported(""));
}
