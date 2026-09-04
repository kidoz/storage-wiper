/**
 * @file SmartService.cpp
 * @brief SMART data retrieval service implementation
 */

#include "helper/services/SmartService.hpp"

#include "util/FileDescriptor.hpp"
#include "util/Logger.hpp"

#include <fcntl.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <scsi/sg.h>

namespace {

// ATA SMART command constants
constexpr uint8_t ATA_SMART_CMD = 0xB0;
constexpr uint8_t ATA_SMART_READ_DATA = 0xD0;
constexpr uint8_t ATA_SMART_RETURN_STATUS = 0xDA;

// Magic LBA mid/high values that select the SMART command set
constexpr uint8_t SMART_LBA_MID_PASS = 0x4F;
constexpr uint8_t SMART_LBA_HIGH_PASS = 0xC2;

// LBA mid/high values returned by SMART RETURN STATUS when the drive has
// exceeded a failure threshold
constexpr uint8_t SMART_LBA_MID_FAIL = 0xF4;
constexpr uint8_t SMART_LBA_HIGH_FAIL = 0x2C;

// SCSI/ATA translation (SAT) pass-through opcodes
constexpr uint8_t SCSI_ATA_PASSTHROUGH_16 = 0x85;
constexpr uint8_t SCSI_ATA_PASSTHROUGH_12 = 0xA1;

// SAT protocol values (bits 1-4 of CDB byte 1)
constexpr uint8_t SAT_PROTOCOL_NON_DATA = 3;
constexpr uint8_t SAT_PROTOCOL_PIO_DATA_IN = 4;

// SAT flag byte (CDB byte 2): T_LENGTH=sector count, BYT_BLOK=blocks, T_DIR=from device
constexpr uint8_t SAT_FLAGS_PIO_DATA_IN = 0x0E;
// SAT flag byte for a non-data command that returns the ATA registers (CK_COND)
constexpr uint8_t SAT_FLAGS_CHECK_CONDITION = 0x20;

// SCSI status returned when the ATA registers come back in the sense buffer
constexpr uint8_t SCSI_STATUS_CHECK_CONDITION = 0x02;

// Descriptor code of the SAT "ATA Status Return" sense descriptor
constexpr uint8_t SAT_ATA_RETURN_DESCRIPTOR = 0x09;

// Timeout for a single SG_IO pass-through command. Generous on purpose: a
// sleeping drive can take tens of seconds to spin up before it answers, and
// smartmontools uses a 60-second default for SCSI commands for that reason.
// Callers are not blocked for this long - a query that outlives the caller's
// own budget deposits its result in DiskService's SmartQueryState, so the value
// appears on the next refresh. The cap is kept below the smartmontools default
// so a query thread on an unresponsive device cannot live for several minutes.
constexpr unsigned int SG_TIMEOUT_MS = 30'000;

// Worst-case number of SG_IO commands per query: two attempts to read the data
// structure (16-byte then 12-byte opcode) plus one health-verdict command.
constexpr unsigned int MAX_SG_COMMANDS = 3;
static_assert(std::chrono::milliseconds{SG_TIMEOUT_MS} * MAX_SG_COMMANDS <= std::chrono::minutes{2},
              "A stalled query thread must not outlive the device it holds open by minutes");

// HDD SMART attribute IDs
constexpr uint8_t ATTR_REALLOCATED_SECTORS = 5;
constexpr uint8_t ATTR_POWER_ON_HOURS = 9;
constexpr uint8_t ATTR_AIRFLOW_TEMPERATURE = 190;
constexpr uint8_t ATTR_TEMPERATURE = 194;
constexpr uint8_t ATTR_CURRENT_PENDING_SECTORS = 197;
constexpr uint8_t ATTR_UNCORRECTABLE_ERRORS = 198;

// SSD wear attribute IDs. All three carry a normalised value that counts down
// from 100 as the flash wears out. They are checked in this order because
// attribute 231 is reused as a temperature reading by some controllers.
constexpr uint8_t ATTR_MEDIA_WEAROUT_INDICATOR = 233;
constexpr uint8_t ATTR_WEAR_LEVELING_COUNT = 177;
constexpr uint8_t ATTR_SSD_LIFE_LEFT = 231;

// SMART attribute table layout inside the 512-byte data structure
constexpr size_t ATTR_TABLE_OFFSET = 2;
constexpr size_t ATTR_ENTRY_SIZE = 12;
constexpr size_t ATTR_COUNT = 30;

// Thresholds for health status
constexpr int WARNING_REALLOCATED_SECTORS = 5;
constexpr int CRITICAL_REALLOCATED_SECTORS = 50;
constexpr int WARNING_PENDING_SECTORS = 1;
constexpr int CRITICAL_PENDING_SECTORS = 10;
constexpr int WARNING_TEMPERATURE = 50;
constexpr int CRITICAL_TEMPERATURE = 60;
constexpr int WARNING_PERCENTAGE_USED = 90;
constexpr int CRITICAL_PERCENTAGE_USED = 100;
constexpr int WARNING_SPARE_MARGIN = 10;

// Plausible temperature range in Celsius; anything outside is treated as unknown
constexpr int MIN_PLAUSIBLE_TEMPERATURE = -40;
constexpr int MAX_PLAUSIBLE_TEMPERATURE = 150;

/**
 * Check if device path looks like NVMe
 */
auto is_nvme_device(std::string_view path) -> bool {
    return path.starts_with("/dev/nvme");
}

/**
 * Check if device path looks like an MMC/SD device
 */
auto is_mmc_device(std::string_view path) -> bool {
    return path.starts_with("/dev/mmcblk");
}

/**
 * Extract the kernel device name from a device path (/dev/sda -> sda)
 */
auto device_name_of(std::string_view path) -> std::string {
    const auto pos = path.rfind('/');
    return std::string{pos == std::string_view::npos ? path : path.substr(pos + 1)};
}

/**
 * Read the first line of a sysfs file
 */
auto read_sysfs_line(const std::string& path) -> std::string {
    std::ifstream file{path};
    if (!file) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    return line;
}

/**
 * Determine whether a block device is a spinning disk.
 * Defaults to true (HDD) when the rotational flag cannot be read, so that
 * SSD-specific attribute interpretation is only applied when it is known to
 * be correct.
 */
auto is_rotational(const std::string& device_path) -> bool {
    const auto value =
        read_sysfs_line(std::format("/sys/block/{}/queue/rotational", device_name_of(device_path)));
    return value != "0";
}

/**
 * Clamp a 64-bit attribute value into the int range used by the model
 */
auto clamp_to_int(int64_t value) -> int {
    if (value < 0) {
        return -1;
    }
    return static_cast<int>(std::min<int64_t>(value, INT32_MAX));
}

/**
 * Issue an ATA SMART command through the legacy HDIO_DRIVE_CMD ioctl.
 *
 * The kernel ABI is: args[0]=command, args[1]=LBA low (sector number),
 * args[2]=feature, args[3]=sector count. A sector count of zero makes the
 * kernel issue a non-data command and return no payload, so SMART READ DATA
 * must ask for exactly one sector.
 */
auto hdio_read_smart_data(int fd, uint8_t* out) -> bool {
    std::array<uint8_t, 4 + SmartService::SMART_DATA_SIZE> buffer{};
    buffer[0] = ATA_SMART_CMD;        // Command register
    buffer[1] = 0;                    // LBA low / sector number
    buffer[2] = ATA_SMART_READ_DATA;  // Feature register (SMART subcommand)
    buffer[3] = 1;                    // Sector count: one 512-byte sector

    if (ioctl(fd, HDIO_DRIVE_CMD, buffer.data()) != 0) {
        return false;
    }

    std::memcpy(out, buffer.data() + 4, SmartService::SMART_DATA_SIZE);
    return true;
}

/**
 * Ask the drive for its overall health verdict through HDIO_DRIVE_TASK.
 *
 * The verdict comes back in the LBA mid/high output registers:
 * 0x4F/0xC2 = PASSED, 0xF4/0x2C = FAILED. HDIO_DRIVE_TASK is required here
 * (not HDIO_DRIVE_CMD) because only it returns the output registers.
 * Layout: [command, feature, nsector, sector, lcyl, hcyl, select].
 *
 * @return true if the verdict was read, with drive_failed set accordingly
 */
auto hdio_smart_status(int fd, bool& drive_failed) -> bool {
    std::array<uint8_t, 7> args{};
    args[0] = ATA_SMART_CMD;
    args[1] = ATA_SMART_RETURN_STATUS;
    args[4] = SMART_LBA_MID_PASS;
    args[5] = SMART_LBA_HIGH_PASS;

    if (ioctl(fd, HDIO_DRIVE_TASK, args.data()) != 0) {
        return false;
    }

    drive_failed = (args[4] == SMART_LBA_MID_FAIL && args[5] == SMART_LBA_HIGH_FAIL);
    return true;
}

/**
 * Build a SAT ATA PASS-THROUGH CDB for a SMART subcommand.
 *
 * @param use_12 Use the 12-byte opcode (0xA1) instead of the 16-byte one (0x85)
 * @param feature SMART subcommand (feature register)
 * @param sector_count Sectors to transfer (0 for non-data commands)
 * @param check_condition Request the ATA output registers in the sense buffer
 */
auto build_ata_passthrough_cdb(bool use_12, uint8_t feature, uint8_t sector_count,
                               bool check_condition) -> std::array<uint8_t, 16> {
    const uint8_t protocol = sector_count > 0 ? SAT_PROTOCOL_PIO_DATA_IN : SAT_PROTOCOL_NON_DATA;
    const uint8_t flags = check_condition ? SAT_FLAGS_CHECK_CONDITION : SAT_FLAGS_PIO_DATA_IN;

    std::array<uint8_t, 16> cdb{};
    if (use_12) {
        cdb[0] = SCSI_ATA_PASSTHROUGH_12;
        cdb[1] = static_cast<uint8_t>(protocol << 1);
        cdb[2] = flags;
        cdb[3] = feature;
        cdb[4] = sector_count;
        cdb[5] = 0;  // LBA low
        cdb[6] = SMART_LBA_MID_PASS;
        cdb[7] = SMART_LBA_HIGH_PASS;
        cdb[8] = 0;  // Device
        cdb[9] = ATA_SMART_CMD;
    } else {
        cdb[0] = SCSI_ATA_PASSTHROUGH_16;
        cdb[1] = static_cast<uint8_t>(protocol << 1);  // EXTEND = 0 (28-bit command)
        cdb[2] = flags;
        cdb[3] = 0;                                    // Feature (high)
        cdb[4] = feature;
        cdb[5] = 0;                                    // Sector count (high)
        cdb[6] = sector_count;
        cdb[7] = 0;                                    // LBA low (high)
        cdb[8] = 0;                                    // LBA low
        cdb[9] = 0;                                    // LBA mid (high)
        cdb[10] = SMART_LBA_MID_PASS;
        cdb[11] = 0;                                   // LBA high (high)
        cdb[12] = SMART_LBA_HIGH_PASS;
        cdb[13] = 0;                                   // Device
        cdb[14] = ATA_SMART_CMD;
    }
    return cdb;
}

/**
 * Extract the ATA LBA mid/high output registers from a descriptor-format
 * sense buffer produced by a CK_COND pass-through command.
 */
auto parse_ata_return_descriptor(const uint8_t* sense, size_t length, uint8_t& lba_mid,
                                 uint8_t& lba_high) -> bool {
    constexpr size_t SENSE_HEADER_SIZE = 8;
    constexpr size_t DESCRIPTOR_MIN_SIZE = 14;

    if (length < SENSE_HEADER_SIZE) {
        return false;
    }

    const uint8_t response_code = sense[0] & 0x7F;
    // Only descriptor-format sense (0x72/0x73) carries the ATA register block.
    if (response_code != 0x72 && response_code != 0x73) {
        return false;
    }

    const size_t additional = sense[7];
    const size_t end = std::min(length, SENSE_HEADER_SIZE + additional);

    size_t offset = SENSE_HEADER_SIZE;
    while (offset + 2 <= end) {
        const uint8_t descriptor_code = sense[offset];
        const size_t descriptor_length = static_cast<size_t>(sense[offset + 1]) + 2;

        if (descriptor_length <= 2) {
            break;  // Malformed descriptor, stop rather than loop forever
        }

        if (descriptor_code == SAT_ATA_RETURN_DESCRIPTOR && offset + DESCRIPTOR_MIN_SIZE <= end) {
            lba_mid = sense[offset + 9];
            lba_high = sense[offset + 11];
            return true;
        }

        offset += descriptor_length;
    }

    return false;
}

/**
 * Read the SMART data structure through SCSI/ATA translation (SG_IO).
 * Used for USB bridges, SAS controllers and other SCSI-attached devices
 * where HDIO_DRIVE_CMD is not available.
 */
auto sat_read_smart_data(int fd, bool use_12, uint8_t* out) -> bool {
    auto cdb = build_ata_passthrough_cdb(use_12, ATA_SMART_READ_DATA, 1, false);
    std::array<uint8_t, 32> sense{};

    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = static_cast<unsigned char>(use_12 ? 12 : 16);
    io.cmdp = cdb.data();
    io.dxfer_len = static_cast<unsigned int>(SmartService::SMART_DATA_SIZE);
    io.dxferp = out;
    io.sbp = sense.data();
    io.mx_sb_len = static_cast<unsigned char>(sense.size());
    io.timeout = SG_TIMEOUT_MS;

    if (ioctl(fd, SG_IO, &io) != 0) {
        return false;
    }

    // Accept only a clean, complete transfer. A CHECK CONDITION means the bridge
    // rejected the pass-through, and a non-zero residual means fewer than 512
    // bytes arrived, leaving the tail of the buffer undefined.
    return io.status == 0 && io.host_status == 0 && (io.driver_status & 0x0F) == 0 && io.resid == 0;
}

/**
 * Read the drive health verdict through SCSI/ATA translation (SG_IO).
 */
auto sat_smart_status(int fd, bool use_12, bool& drive_failed) -> bool {
    auto cdb = build_ata_passthrough_cdb(use_12, ATA_SMART_RETURN_STATUS, 0, true);
    std::array<uint8_t, 64> sense{};

    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_NONE;
    io.cmd_len = static_cast<unsigned char>(use_12 ? 12 : 16);
    io.cmdp = cdb.data();
    io.sbp = sense.data();
    io.mx_sb_len = static_cast<unsigned char>(sense.size());
    io.timeout = SG_TIMEOUT_MS;

    if (ioctl(fd, SG_IO, &io) != 0) {
        return false;
    }

    // With CK_COND set the device reports CHECK CONDITION and returns the ATA
    // output registers in the sense buffer.
    if (io.status != 0 && io.status != SCSI_STATUS_CHECK_CONDITION) {
        return false;
    }

    uint8_t lba_mid = 0;
    uint8_t lba_high = 0;
    if (!parse_ata_return_descriptor(sense.data(), sense.size(), lba_mid, lba_high)) {
        return false;
    }

    drive_failed = (lba_mid == SMART_LBA_MID_FAIL && lba_high == SMART_LBA_HIGH_FAIL);
    return true;
}

/**
 * Parse an "0x0A" style hex token used by the MMC sysfs attributes
 *
 * @return Parsed value, or -1 if the token is empty or not valid hexadecimal
 */
auto parse_hex_token(std::string_view token) -> int {
    constexpr size_t MAX_HEX_DIGITS = 7;  // Stays well inside the int range

    if (token.starts_with("0x") || token.starts_with("0X")) {
        token.remove_prefix(2);
    }

    if (token.empty() || token.size() > MAX_HEX_DIGITS) {
        return -1;
    }

    int value = 0;
    for (const char character : token) {
        int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        } else {
            return -1;
        }
        value = (value * 16) + digit;
    }

    return value;
}

}  // namespace

auto SmartService::get_smart_data(const std::string& device_path) -> SmartData {
    SmartData result;

    if (!is_smart_supported(device_path)) {
        return result;
    }

    if (is_nvme_device(device_path)) {
        result = read_nvme_smart(device_path);
    } else if (is_mmc_device(device_path)) {
        result = read_mmc_health(device_path);
    } else {
        result = read_ata_smart(device_path);
    }

    if (result.available) {
        result.status = calculate_health_status(result);
    }

    return result;
}

auto SmartService::is_smart_supported(const std::string& device_path) -> bool {
    // Virtual devices have no health data
    if (device_path.starts_with("/dev/loop") || device_path.starts_with("/dev/vd") ||
        device_path.starts_with("/dev/dm-") || device_path.starts_with("/dev/zram") ||
        device_path.starts_with("/dev/ram")) {
        return false;
    }

    // SCSI/SATA/IDE (including USB bridges that speak SAT), NVMe, and MMC.
    // Plain SD cards land here too but simply report nothing: they have no
    // EXT_CSD life-time registers.
    return device_path.starts_with("/dev/sd") || device_path.starts_with("/dev/nvme") ||
           device_path.starts_with("/dev/hd") || device_path.starts_with("/dev/mmcblk");
}

auto SmartService::is_checksum_valid(const uint8_t* data) -> bool {
    uint8_t sum = 0;
    for (size_t i = 0; i < SMART_DATA_SIZE; ++i) {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    return sum == 0;
}

auto SmartService::has_attributes(const uint8_t* data) -> bool {
    for (size_t i = 0; i < ATTR_COUNT; ++i) {
        if (data[ATTR_TABLE_OFFSET + (i * ATTR_ENTRY_SIZE)] != 0) {
            return true;
        }
    }
    return false;
}

auto SmartService::parse_ata_attribute(const uint8_t* data, uint8_t attr_id) -> int64_t {
    // Each attribute entry is 12 bytes:
    // [0] = attribute ID
    // [1-2] = flags
    // [3] = current (normalised) value
    // [4] = worst value
    // [5-10] = raw value (6 bytes, little-endian)
    // [11] = reserved
    for (size_t i = 0; i < ATTR_COUNT; ++i) {
        const size_t offset = ATTR_TABLE_OFFSET + (i * ATTR_ENTRY_SIZE);

        // Empty slots are skipped: the table is not guaranteed to be contiguous
        if (data[offset] != attr_id || attr_id == 0) {
            continue;
        }

        // Lower 32 bits of the raw value. The upper two raw bytes are
        // vendor-specific on most attributes and are deliberately ignored.
        const uint32_t raw = static_cast<uint32_t>(data[offset + 5]) |
                             (static_cast<uint32_t>(data[offset + 6]) << 8) |
                             (static_cast<uint32_t>(data[offset + 7]) << 16) |
                             (static_cast<uint32_t>(data[offset + 8]) << 24);
        return static_cast<int64_t>(raw);
    }

    return -1;  // Attribute not found
}

auto SmartService::parse_ata_attribute_value(const uint8_t* data, uint8_t attr_id) -> int {
    for (size_t i = 0; i < ATTR_COUNT; ++i) {
        const size_t offset = ATTR_TABLE_OFFSET + (i * ATTR_ENTRY_SIZE);

        if (data[offset] != attr_id || attr_id == 0) {
            continue;
        }

        return static_cast<int>(data[offset + 3]);
    }

    return -1;
}

void SmartService::parse_ata_smart_data(const uint8_t* data, bool rotational, SmartData& result) {
    result.reallocated_sectors = clamp_to_int(parse_ata_attribute(data, ATTR_REALLOCATED_SECTORS));
    result.power_on_hours = parse_ata_attribute(data, ATTR_POWER_ON_HOURS);
    result.pending_sectors = clamp_to_int(parse_ata_attribute(data, ATTR_CURRENT_PENDING_SECTORS));
    result.uncorrectable_errors =
        clamp_to_int(parse_ata_attribute(data, ATTR_UNCORRECTABLE_ERRORS));

    // Temperature lives in the low byte of the raw value; the upper bytes hold
    // vendor-specific minimum/maximum readings and must be masked off.
    auto temperature_from = [data](uint8_t attr_id) -> int {
        const auto raw = parse_ata_attribute(data, attr_id);
        if (raw < 0) {
            return -1;
        }
        const auto celsius = static_cast<int>(raw & 0xFF);
        if (celsius < MIN_PLAUSIBLE_TEMPERATURE || celsius > MAX_PLAUSIBLE_TEMPERATURE) {
            return -1;
        }
        return celsius;
    };

    result.temperature_celsius = temperature_from(ATTR_TEMPERATURE);
    if (result.temperature_celsius < 0) {
        result.temperature_celsius = temperature_from(ATTR_AIRFLOW_TEMPERATURE);
    }

    if (rotational) {
        return;
    }

    // SSD wear: the normalised value of these attributes counts down from 100
    // as the flash is consumed. Attribute 231 is checked last because a few
    // controllers reuse that ID for a temperature reading.
    for (const uint8_t attr_id :
         {ATTR_MEDIA_WEAROUT_INDICATOR, ATTR_WEAR_LEVELING_COUNT, ATTR_SSD_LIFE_LEFT}) {
        const int life_left = parse_ata_attribute_value(data, attr_id);
        if (life_left >= 0 && life_left <= 100) {
            result.percentage_used = 100 - life_left;
            break;
        }
    }
}

auto SmartService::read_ata_smart(const std::string& device_path) -> SmartData {
    SmartData result;

    const util::FileDescriptor device{open(device_path.c_str(), O_RDONLY | O_NONBLOCK)};
    if (!device) {
        return result;
    }
    const int fd = device.get();

    std::array<uint8_t, SMART_DATA_SIZE> data{};

    // Try the direct ATA ioctl first, then SCSI/ATA translation for devices
    // behind a USB bridge or SAS/SCSI controller.
    bool have_data = hdio_read_smart_data(fd, data.data());
    bool use_sat_12 = false;
    bool via_sat = false;

    if (!have_data) {
        have_data = sat_read_smart_data(fd, false, data.data());
        via_sat = have_data;
    }
    if (!have_data) {
        have_data = sat_read_smart_data(fd, true, data.data());
        via_sat = have_data;
        use_sat_12 = have_data;
    }

    if (!have_data) {
        return result;
    }

    // Never report an empty or obviously bogus structure as valid health data.
    if (!has_attributes(data.data())) {
        LOG_WARNING("SmartService",
                    std::format("SMART data for {} contains no attributes", device_path));
        return result;
    }

    // A bad checksum means the structure is corrupt or truncated. Parsing it
    // anyway could turn garbage into a "Good" verdict, so the data is discarded.
    if (!is_checksum_valid(data.data())) {
        LOG_WARNING("SmartService",
                    std::format("SMART data checksum mismatch for {}, discarding", device_path));
        return result;
    }

    result.available = true;
    result.healthy = true;

    parse_ata_smart_data(data.data(), is_rotational(device_path), result);

    // Overall health verdict from the drive itself
    bool drive_failed = false;
    bool have_verdict = via_sat ? sat_smart_status(fd, use_sat_12, drive_failed)
                                : hdio_smart_status(fd, drive_failed);
    if (!have_verdict && !via_sat) {
        have_verdict = sat_smart_status(fd, false, drive_failed);
    }
    if (have_verdict && drive_failed) {
        result.healthy = false;  // Drive reports threshold-exceeded failure
    }

    return result;
}

auto SmartService::read_nvme_smart(const std::string& device_path) -> SmartData {
    SmartData result;

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
    static_assert(sizeof(nvme_smart_log) == 512, "NVMe SMART log must be 512 bytes");

    nvme_smart_log smart_log{};

    // Passing an admin command through the namespace block device is deprecated
    // in the kernel, so fall back to the controller character device
    // (/dev/nvme0n1 -> /dev/nvme0) if the namespace path does not work.
    std::vector<std::string> candidates{device_path};
    if (const auto name_end = device_path.find('n', std::strlen("/dev/nvme"));
        name_end != std::string::npos) {
        candidates.push_back(device_path.substr(0, name_end));
    }

    for (const auto& path : candidates) {
        const util::FileDescriptor device{open(path.c_str(), O_RDONLY)};
        if (!device) {
            continue;
        }

        struct nvme_admin_cmd cmd{};
        cmd.opcode = 0x02;  // Get Log Page
        cmd.nsid = 0xFFFF'FFFF;
        cmd.addr = reinterpret_cast<uint64_t>(&smart_log);
        cmd.data_len = sizeof(smart_log);
        cmd.cdw10 = 0x02 | (((sizeof(smart_log) / 4) - 1) << 16);  // Log ID 2, NUMDL

        if (ioctl(device.get(), NVME_IOCTL_ADMIN_CMD, &cmd) == 0) {
            result.available = true;
            break;
        }
    }

    if (!result.available) {
        return result;
    }

    // Composite temperature is reported in Kelvin
    const int temp_kelvin = smart_log.temperature;
    if (temp_kelvin > 0) {
        const int celsius = temp_kelvin - 273;
        if (celsius >= MIN_PLAUSIBLE_TEMPERATURE && celsius <= MAX_PLAUSIBLE_TEMPERATURE) {
            result.temperature_celsius = celsius;
        }
    }

    // Power on hours (lower 64 bits of the 128-bit counter)
    uint64_t poh = 0;
    std::memcpy(&poh, smart_log.power_on_hours, sizeof(poh));
    result.power_on_hours = static_cast<int64_t>(poh);

    // Media and data integrity errors
    uint64_t media_errs = 0;
    std::memcpy(&media_errs, smart_log.media_errors, sizeof(media_errs));
    result.uncorrectable_errors = clamp_to_int(
        static_cast<int64_t>(std::min<uint64_t>(media_errs, static_cast<uint64_t>(INT32_MAX))));

    // Wear indicators
    result.percentage_used = smart_log.percent_used;
    result.available_spare_percent = smart_log.avail_spare;
    result.available_spare_threshold_percent = smart_log.spare_thresh;

    // Critical warning bits: spare below threshold, temperature, degraded
    // reliability, read-only media, failed volatile memory backup
    result.healthy = (smart_log.critical_warning == 0);

    return result;
}

auto SmartService::read_mmc_health(const std::string& device_path) -> SmartData {
    SmartData result;

    const auto sysfs_dir = std::format("/sys/block/{}/device", device_name_of(device_path));

    // EXT_CSD DEVICE_LIFE_TIME_EST_TYP_A/B: "0x01" = 0-10% used, up to
    // "0x0A" = 90-100% used, "0x0B" = exceeded its maximum estimated lifetime
    const auto life_time = read_sysfs_line(std::format("{}/life_time", sysfs_dir));
    if (!life_time.empty()) {
        int worst = -1;
        size_t pos = 0;
        while (pos < life_time.size()) {
            const auto space = life_time.find(' ', pos);
            const auto token = std::string_view{life_time}.substr(
                pos, space == std::string::npos ? std::string::npos : space - pos);
            if (const int value = parse_hex_token(token); value >= 1 && value <= 11) {
                worst = std::max(worst, value);
            }
            if (space == std::string::npos) {
                break;
            }
            pos = space + 1;
        }

        if (worst >= 1) {
            result.available = true;
            result.percentage_used = std::min((worst - 1) * 10, 100);
        }
    }

    // EXT_CSD PRE_EOL_INFO: 0x01 normal, 0x02 warning (80% of reserved blocks
    // consumed), 0x03 urgent
    const auto pre_eol = read_sysfs_line(std::format("{}/pre_eol_info", sysfs_dir));
    if (!pre_eol.empty()) {
        const int value = parse_hex_token(pre_eol);
        if (value >= 1) {
            result.available = true;
            if (value >= 3) {
                result.healthy = false;
            } else if (value == 2) {
                result.percentage_used = std::max(result.percentage_used, WARNING_PERCENTAGE_USED);
            }
        }
    }

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
        data.temperature_celsius >= CRITICAL_TEMPERATURE ||
        data.percentage_used >= CRITICAL_PERCENTAGE_USED) {
        return SmartData::HealthStatus::CRITICAL;
    }

    // Spare capacity below the vendor threshold means the drive is out of
    // replacement blocks
    if (data.available_spare_percent >= 0 && data.available_spare_threshold_percent >= 0 &&
        data.available_spare_percent < data.available_spare_threshold_percent) {
        return SmartData::HealthStatus::CRITICAL;
    }

    // Check for warning conditions
    if (data.reallocated_sectors >= WARNING_REALLOCATED_SECTORS ||
        data.pending_sectors >= WARNING_PENDING_SECTORS ||
        data.temperature_celsius >= WARNING_TEMPERATURE || data.uncorrectable_errors > 0 ||
        data.percentage_used >= WARNING_PERCENTAGE_USED) {
        return SmartData::HealthStatus::WARNING;
    }

    if (data.available_spare_percent >= 0 && data.available_spare_threshold_percent >= 0 &&
        data.available_spare_percent <
            data.available_spare_threshold_percent + WARNING_SPARE_MARGIN) {
        return SmartData::HealthStatus::WARNING;
    }

    return SmartData::HealthStatus::GOOD;
}
