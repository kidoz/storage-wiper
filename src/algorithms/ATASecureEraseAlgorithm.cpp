/**
 * @file ATASecureEraseAlgorithm.cpp
 * @brief Implementation of ATA Secure Erase using Linux ioctl
 */

#include "algorithms/ATASecureEraseAlgorithm.hpp"

#include "algorithms/NvmeSanitize.hpp"
#include "models/WipeTypes.hpp"
#include "util/FileDescriptor.hpp"

#include <fcntl.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <thread>

#include <scsi/sg.h>

namespace {

// ATA commands that transfer a 512-byte security structure need PIO Data-Out.
// HDIO_DRIVE_CMD only implements PIO Data-In and a fixed 10-second timeout, so
// the security commands go through SG_IO ATA PASS-THROUGH(16) instead.
constexpr uint8_t ATA_PASS_THROUGH_16 = 0x85;
constexpr uint8_t SAT_PROTOCOL_NON_DATA = 3;
constexpr uint8_t SAT_PROTOCOL_PIO_DATA_OUT = 5;
// CDB byte 2: T_LENGTH = sector-count field (2), BYT_BLOK = blocks (1),
// T_DIR clear for a to-device transfer (bit 3).
constexpr uint8_t SAT_FLAGS_NON_DATA = 0x00;
constexpr uint8_t SAT_FLAGS_PIO_DATA_OUT = 0x06;
constexpr unsigned int ATA_SECURITY_TIMEOUT_MS = 30'000;
constexpr uint64_t SECURITY_PAYLOAD_SIZE = 512;
constexpr uint64_t MAX_ATA_ERASE_TIMEOUT_MS = 24ULL * 60 * 60 * 1'000;

/**
 * Issue an ATA SECURITY command, optionally with its 512-byte data-out
 * structure. Returns true only for a clean, complete transfer.
 */
auto ata_security_command(int fd, uint8_t command, const uint8_t* data_out, unsigned int timeout_ms)
    -> bool {
    const bool has_data = data_out != nullptr;
    std::array<uint8_t, 16> cdb{};
    cdb[0] = ATA_PASS_THROUGH_16;
    cdb[1] =
        static_cast<uint8_t>((has_data ? SAT_PROTOCOL_PIO_DATA_OUT : SAT_PROTOCOL_NON_DATA) << 1U);
    cdb[2] = has_data ? SAT_FLAGS_PIO_DATA_OUT : SAT_FLAGS_NON_DATA;
    cdb[5] = 0;                 // sector count high
    cdb[6] = has_data ? 1 : 0;  // sector count low: one 512-byte block
    cdb[14] = command;
    cdb[15] = 0;                // control

    std::array<uint8_t, 32> sense{};
    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.dxfer_direction = has_data ? SG_DXFER_TO_DEV : SG_DXFER_NONE;
    io.cmd_len = static_cast<unsigned char>(cdb.size());
    io.cmdp = cdb.data();
    io.dxfer_len = has_data ? static_cast<unsigned int>(SECURITY_PAYLOAD_SIZE) : 0;
    io.dxferp = has_data ? const_cast<uint8_t*>(data_out) : nullptr;
    io.sbp = sense.data();
    io.mx_sb_len = static_cast<unsigned char>(sense.size());
    io.timeout = timeout_ms;

    if (ioctl(fd, SG_IO, &io) != 0) {
        return false;
    }
    return io.status == 0 && io.host_status == 0 && (io.driver_status & 0x0FU) == 0 &&
           io.resid == 0;
}

auto ata_erase_timeout_ms(const ATASecurityInfo& info, bool enhanced) -> unsigned int {
    // IDENTIFY words 89/90 report the normal/enhanced erase time in 2-minute
    // units. Use the longer estimate with a margin, and cap it at one day.
    const uint64_t normal = static_cast<uint64_t>(info.erase_time_normal) * 2ULL * 60ULL * 1'000ULL;
    const uint64_t enhanced_time =
        static_cast<uint64_t>(info.erase_time_enhanced) * 2ULL * 60ULL * 1'000ULL;
    const uint64_t selected = enhanced ? enhanced_time : normal;
    const uint64_t estimate = selected == 0 ? 60ULL * 60ULL * 1'000ULL : selected;
    const uint64_t with_margin = estimate + 5ULL * 60ULL * 1'000ULL;
    return static_cast<unsigned int>(std::min(with_margin, MAX_ATA_ERASE_TIMEOUT_MS));
}

}  // namespace

bool ATASecureEraseAlgorithm::execute([[maybe_unused]] int fd, [[maybe_unused]] uint64_t size,
                                      ProgressCallback callback,
                                      [[maybe_unused]] const std::atomic<bool>& cancel_flag) {
    // This method should not be called for hardware secure erase
    // Use execute_on_device instead
    report_progress(callback, 0, "Error: Use execute_on_device for hardware secure erase", true,
                    true, "Hardware secure erase requires device path, not file descriptor");
    return false;
}

bool ATASecureEraseAlgorithm::execute_on_device(const std::string& device_path,
                                                [[maybe_unused]] uint64_t size,
                                                ProgressCallback callback,
                                                const std::atomic<bool>& cancel_flag) {
    if (device_path.starts_with("/dev/nvme") && !nvme_sanitize::controller_path_of(device_path)) {
        report_progress(callback, 0, "Error", true, true,
                        "Cannot determine the NVMe namespace device for " + device_path);
        return false;
    }
    const util::FileDescriptor fd(open(device_path.c_str(), O_RDWR | O_NONBLOCK));
    if (!fd) {
        report_progress(callback, 0, "Error", true, true,
                        "Failed to open device: " + std::string(strerror(errno)));
        return false;
    }
    const auto plan = prepare_firmware(fd.get(), device_path);
    if (!plan) {
        report_progress(callback, 0, "Error", true, true, plan.error().message);
        return false;
    }
    return execute_prepared(fd.get(), *plan, std::move(callback), cancel_flag);
}

auto ATASecureEraseAlgorithm::prepare_firmware(int fd, const std::string& device_path)
    -> std::expected<FirmwarePlan, util::Error> {
    if (device_path.starts_with("/dev/nvme")) {
        if (!nvme_sanitize::controller_path_of(device_path)) {
            return std::unexpected(util::Error{"Cannot determine the NVMe namespace device"});
        }
        return prepare_nvme(fd);
    }
    return prepare_ata(fd);
}

auto ATASecureEraseAlgorithm::prepare_nvme(int fd) -> std::expected<FirmwarePlan, util::Error> {
    const int namespace_id = ioctl(fd, NVME_IOCTL_ID);
    if (namespace_id <= 0) {
        return std::unexpected(util::Error{"Failed to read the NVMe namespace ID"});
    }
    std::array<uint8_t, 4'096> identify{};
    if (const int result = nvme_identify_controller(fd, identify.data()); result != 0) {
        return std::unexpected(
            util::Error{"Failed to read NVMe capabilities: " + nvme_error_text(result)});
    }
    const auto caps = nvme_sanitize::parse_capabilities(identify.data());
    const auto selected = nvme_sanitize::choose_erase_plan(caps);
    FirmwarePlan plan{.method = "", .scope = "nvme-subsystem", .cancellable = false};
    switch (selected) {
        case nvme_sanitize::ErasePlan::SANITIZE_CRYPTO_ERASE:
            plan.method = "nvme-sanitize-crypto";
            break;
        case nvme_sanitize::ErasePlan::SANITIZE_BLOCK_ERASE:
            plan.method = "nvme-sanitize-block";
            break;
        case nvme_sanitize::ErasePlan::SANITIZE_OVERWRITE:
            plan.method = "nvme-sanitize-overwrite";
            break;
        case nvme_sanitize::ErasePlan::FORMAT_CRYPTO_ERASE:
            plan.method = "nvme-format-crypto";
            // With SES != 0 the specification uses FNA bit 1, ignoring bit 0.
            plan.scope = caps.format_secure_erase_all_namespaces ? "nvme-subsystem" : "device";
            break;
        case nvme_sanitize::ErasePlan::UNSUPPORTED:
            return std::unexpected(util::Error{
                "This NVMe controller supports neither Sanitize nor cryptographic Format NVM"});
    }
    if (plan.scope == "nvme-subsystem" && (caps.namespace_count != 1 || namespace_id != 1)) {
        return std::unexpected(util::Error{
            "Subsystem-wide firmware erase requires exactly one allocated namespace (NSID 1)"});
    }
    if (plan.scope == "nvme-subsystem") {
        // Sysfs can only enumerate controllers attached to this host. CMIC and
        // NMIC also reject subsystems/namespaces that can be shared elsewhere.
        constexpr size_t ID_CTRL_CMIC = 76;
        constexpr size_t ID_NS_NMIC = 30;
        if ((identify[ID_CTRL_CMIC] & 0x03U) != 0) {
            return std::unexpected(util::Error{
                "Subsystem-wide erase of multi-port or multi-controller NVMe is unsupported"});
        }
        std::array<uint8_t, 4'096> namespace_identify{};
        if (nvme_identify_namespace(fd, static_cast<uint32_t>(namespace_id),
                                    namespace_identify.data()) != 0 ||
            (namespace_identify[ID_NS_NMIC] & 0x01U) != 0) {
            return std::unexpected(util::Error{
                "Cannot establish an unshared NVMe namespace for subsystem-wide erase"});
        }
    }
    return plan;
}

auto ATASecureEraseAlgorithm::prepare_ata(int fd) -> std::expected<FirmwarePlan, util::Error> {
    const auto info = get_security_info(fd);
    if (!info.supported) {
        return std::unexpected(util::Error{"Device does not support ATA Security"});
    }
    if (info.frozen || info.locked || info.count_expired || info.enabled) {
        return std::unexpected(util::Error{
            "ATA security is frozen, locked, already enabled, or its attempt count expired"});
    }
    return FirmwarePlan{.method = info.enhanced_erase_supported ? "ata-enhanced-security-erase"
                                                                : "ata-security-erase",
                        .scope = "device",
                        .cancellable = false};
}

bool ATASecureEraseAlgorithm::execute_prepared(int fd, const FirmwarePlan& plan,
                                               ProgressCallback callback,
                                               const std::atomic<bool>& cancel_flag) {
    if (cancel_flag.load()) {
        report_progress(callback, 0, "Cancelled", true, true, "Operation was cancelled by user");
        return false;
    }
    // Reprobe through the claimed descriptor. A changed method or scope requires
    // a new confirmation; never silently switch firmware operations.
    const bool nvme = plan.method.starts_with("nvme-");
    const auto current = nvme ? prepare_nvme(fd) : prepare_ata(fd);
    if (!current || *current != plan) {
        report_progress(callback, 0, "Error", true, true,
                        current ? "Firmware erase plan changed; confirm the device again"
                                : current.error().message);
        return false;
    }
    return nvme ? nvme_secure_erase(fd, plan, callback, cancel_flag)
                : ata_secure_erase(fd, plan, callback, cancel_flag);
}

bool ATASecureEraseAlgorithm::ata_secure_erase(int fd, const FirmwarePlan& plan,
                                               ProgressCallback& callback,
                                               const std::atomic<bool>& cancel_flag) {
    const auto security_info = get_security_info(fd);
    // Check for cancellation
    if (cancel_flag.load()) {
        report_progress(callback, 0, "Cancelled", true, true, "Operation was cancelled by user");
        return false;
    }

    // Calculate estimated time
    int estimated_minutes = 0;
    if (security_info.enhanced_erase_supported && security_info.erase_time_enhanced > 0) {
        estimated_minutes = security_info.erase_time_enhanced * 2;
    } else if (security_info.erase_time_normal > 0) {
        estimated_minutes = security_info.erase_time_normal * 2;
    }

    std::string time_msg;
    if (estimated_minutes > 0) {
        if (estimated_minutes >= 60) {
            time_msg = " (estimated: " + std::to_string(estimated_minutes / 60) + "h " +
                       std::to_string(estimated_minutes % 60) + "m)";
        } else {
            time_msg = " (estimated: " + std::to_string(estimated_minutes) + " minutes)";
        }
    }

    report_progress(callback, 5, "Setting temporary security password...");

    // Step 1: Set security password
    if (!set_security_password(fd, TEMP_PASSWORD, false)) {
        report_progress(callback, 5, "Error", true, true,
                        "Failed to set security password. The device may not accept "
                        "password commands or may require specific conditions.");
        return false;
    }

    constexpr auto RECOVERY_WARNING =
        " The temporary ATA security password could not be disabled, or its removal could not be "
        "confirmed. The drive may remain security-enabled; recovery may require the temporary "
        "password 'StorageWiper'.";

    auto report_cancellation = [&](double percentage, const char* cleaned_status) {
        const bool cleared = clear_security_password(fd);
        report_progress(
            callback, percentage, cleared ? cleaned_status : "Cancellation incomplete", true, true,
            cleared ? "Operation was cancelled by user"
                    : "Operation was cancelled by user." + std::string{RECOVERY_WARNING});
    };

    if (cancel_flag.load()) {
        report_cancellation(5, "Cancelled - password disabled");
        return false;
    }

    report_progress(callback, 10, "Preparing for secure erase...");

    // Step 2: Security erase prepare
    if (!security_erase_prepare(fd)) {
        std::string message =
            "Failed to prepare for security erase. The device rejected the SECURITY ERASE PREPARE "
            "command.";
        if (!clear_security_password(fd)) {
            message += RECOVERY_WARNING;
        }
        report_progress(callback, 10, "Error", true, true, message);
        return false;
    }

    if (cancel_flag.load()) {
        report_cancellation(10, "Cancelled");
        return false;
    }

    report_progress(callback, 15, "Starting ATA Secure Erase" + time_msg);

    // Step 3: Execute secure erase
    // This command can take a very long time (minutes to hours)
    const bool use_enhanced = plan.method == "ata-enhanced-security-erase";
    auto start_time = std::chrono::steady_clock::now();

    if (!security_erase_unit(fd, TEMP_PASSWORD, use_enhanced, false)) {
        std::string message =
            "Secure erase command failed. The device may have timed out, rejected "
            "the command, or encountered a hardware error. Check dmesg for more "
            "information.";
        if (!clear_security_password(fd)) {
            message += RECOVERY_WARNING;
        }
        report_progress(callback, 15, "Error", true, true, message);
        return false;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    // Keep using the claimed descriptor even if its device path is recycled.
    const auto post_info = get_security_info(fd);
    if (!post_info.supported || post_info.enabled) {
        std::string message = "Could not confirm ATA security was disabled after erase";
        if (post_info.enabled && !clear_security_password(fd)) {
            message += RECOVERY_WARNING;
        }
        report_progress(callback, 100, "Error", true, true, message);
        return false;
    }

    std::string completion_msg = "ATA Secure Erase completed successfully in " +
                                 std::to_string(duration.count()) + " seconds";
    if (use_enhanced) {
        completion_msg += " (enhanced mode)";
    }

    report_progress(callback, 100, completion_msg, true, false, "");
    return true;
}

ATASecurityInfo ATASecureEraseAlgorithm::get_security_info(const std::string& device_path) {
    const util::FileDescriptor fd(open(device_path.c_str(), O_RDONLY | O_NONBLOCK));
    return fd ? get_security_info(fd.get()) : ATASecurityInfo{};
}

ATASecurityInfo ATASecureEraseAlgorithm::get_security_info(int fd) {
    ATASecurityInfo info{};

    // Try to get IDENTIFY data using hdparm-style ioctl
    uint16_t identify_data[256];
    std::memset(identify_data, 0, sizeof(identify_data));

    // Use HDIO_GET_IDENTITY ioctl
    struct hd_driveid* drive_id = reinterpret_cast<struct hd_driveid*>(identify_data);
    if (ioctl(fd, HDIO_GET_IDENTITY, drive_id) == 0) {
        // Parse security word (word 128)
        uint16_t security_word = identify_data[SECURITY_WORD];

        const auto security = static_cast<uint32_t>(security_word);
        info.supported = (security & SECURITY_SUPPORTED) != 0;
        info.enabled = (security & SECURITY_ENABLED) != 0;
        info.locked = (security & SECURITY_LOCKED) != 0;
        info.frozen = (security & SECURITY_FROZEN) != 0;
        info.count_expired = (security & SECURITY_COUNT_EXPIRED) != 0;
        info.enhanced_erase_supported = (security & SECURITY_ENHANCED_ERASE) != 0;

        // Get erase times (words 89 and 90)
        info.erase_time_normal = identify_data[ERASE_TIME_WORD];
        info.erase_time_enhanced = identify_data[ENHANCED_ERASE_TIME_WORD];
        info.master_password_revision =
            static_cast<uint8_t>(identify_data[MASTER_PASSWORD_REV_WORD] & 0xFFU);

        // Determine state
        if (!info.supported) {
            info.state = ATASecurityState::NOT_SUPPORTED;
        } else if (info.frozen) {
            info.state = ATASecurityState::FROZEN;
        } else if (info.count_expired) {
            info.state = ATASecurityState::EXPIRED;
        } else if (info.locked) {
            info.state = ATASecurityState::ENABLED_LOCKED;
        } else if (info.enabled) {
            info.state = ATASecurityState::ENABLED_UNLOCKED;
        } else {
            info.state = ATASecurityState::DISABLED;
        }
    }

    return info;
}

bool ATASecureEraseAlgorithm::is_device_frozen(const std::string& device_path) {
    ATASecurityInfo info = get_security_info(device_path);
    return info.frozen;
}

bool ATASecureEraseAlgorithm::set_security_password(int fd, const char* password, bool master) {
    // Build the security password structure
    // The structure is 512 bytes:
    // - Word 0: Control word (bit 0 = identifier: 0=user, 1=master)
    // - Words 1-16: Password (32 bytes, null-padded)
    // - Rest: Reserved

    uint8_t buffer[512];
    std::memset(buffer, 0, sizeof(buffer));

    // Control word
    uint16_t control = master ? 0x0001 : 0x0000;
    buffer[0] = static_cast<uint8_t>(static_cast<uint32_t>(control) & 0xFFU);
    buffer[1] = static_cast<uint8_t>((static_cast<uint32_t>(control) >> 8U) & 0xFFU);

    // Copy password (max 32 bytes)
    size_t pwd_len = std::strlen(password);
    size_t copy_len = std::min(pwd_len, size_t(32));
    std::memcpy(&buffer[2], password, copy_len);

    return ata_security_command(fd, ATA_OP_SECURITY_SET_PASSWORD, buffer, ATA_SECURITY_TIMEOUT_MS);
}

bool ATASecureEraseAlgorithm::disable_security_password(int fd, const char* password, bool master) {
    uint8_t buffer[512];
    std::memset(buffer, 0, sizeof(buffer));

    uint16_t control = master ? 0x0001 : 0x0000;
    buffer[0] = static_cast<uint8_t>(static_cast<uint32_t>(control) & 0xFFU);
    buffer[1] = static_cast<uint8_t>((static_cast<uint32_t>(control) >> 8U) & 0xFFU);

    size_t pwd_len = std::strlen(password);
    size_t copy_len = std::min(pwd_len, size_t(32));
    std::memcpy(&buffer[2], password, copy_len);

    return ata_security_command(fd, ATA_OP_SECURITY_DISABLE_PASSWORD, buffer,
                                ATA_SECURITY_TIMEOUT_MS);
}

bool ATASecureEraseAlgorithm::clear_security_password(int fd) {
    if (!disable_security_password(fd, TEMP_PASSWORD, false)) {
        return false;
    }
    const auto current = get_security_info(fd);
    return current.supported && !current.enabled;
}

bool ATASecureEraseAlgorithm::security_erase_prepare(int fd) {
    return ata_security_command(fd, ATA_OP_SECURITY_ERASE_PREPARE, nullptr,
                                ATA_SECURITY_TIMEOUT_MS);
}

bool ATASecureEraseAlgorithm::security_erase_unit(int fd, const char* password, bool enhanced,
                                                  bool master) {
    uint8_t buffer[512];
    std::memset(buffer, 0, sizeof(buffer));

    // Control word:
    // Bit 0: 0=user password, 1=master password
    // Bit 1: 0=normal erase, 1=enhanced erase
    uint16_t control = 0;
    if (master)
        control |= 0x0001U;
    if (enhanced)
        control |= 0x0002U;

    buffer[0] = static_cast<uint8_t>(static_cast<uint32_t>(control) & 0xFFU);
    buffer[1] = static_cast<uint8_t>((static_cast<uint32_t>(control) >> 8U) & 0xFFU);

    size_t pwd_len = std::strlen(password);
    size_t copy_len = std::min(pwd_len, size_t(32));
    std::memcpy(&buffer[2], password, copy_len);

    // The erase runs inside the drive and can take minutes to hours. Derive the
    // SG_IO timeout from the drive's own IDENTIFY estimate so a slow erase is
    // not reported as a failure while the firmware is still working.
    const auto info = get_security_info(fd);
    return ata_security_command(fd, ATA_OP_SECURITY_ERASE_UNIT, buffer,
                                ata_erase_timeout_ms(info, enhanced));
}

void ATASecureEraseAlgorithm::report_progress(ProgressCallback& callback, double percentage,
                                              const std::string& status, bool complete, bool error,
                                              const std::string& error_msg) {
    if (!callback)
        return;

    WipeProgress progress{};
    progress.bytes_written = 0;
    progress.total_bytes = 0;
    progress.current_pass = 1;
    progress.total_passes = 1;
    progress.percentage = percentage;
    progress.status = status;
    progress.is_complete = complete;
    progress.has_error = error;
    progress.error_message = error_msg;

    callback(progress);
}

std::string ATASecureEraseAlgorithm::nvme_error_text(int ioctl_result) {
    // The NVMe passthrough ioctl reports a rejected command by returning the
    // controller's status code as a positive value, leaving errno untouched.
    // Printing strerror(errno) there prints an unrelated earlier error.
    if (ioctl_result > 0) {
        return std::format("NVMe status 0x{:04x}", ioctl_result);
    }
    return strerror(errno);
}

int ATASecureEraseAlgorithm::nvme_identify_controller(int fd, uint8_t* out) {
    nvme_admin_cmd cmd{};
    cmd.opcode = 0x06;  // Identify
    cmd.nsid = 0;
    cmd.addr = reinterpret_cast<uint64_t>(out);
    cmd.data_len = 4'096;
    cmd.cdw10 = 1;  // CNS = 01h: Identify Controller data structure

    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

int ATASecureEraseAlgorithm::nvme_identify_namespace(int fd, uint32_t nsid, uint8_t* out) {
    nvme_admin_cmd cmd{};
    cmd.opcode = 0x06;  // Identify
    cmd.nsid = nsid;
    cmd.addr = reinterpret_cast<uint64_t>(out);
    cmd.data_len = 4'096;
    cmd.cdw10 = 0;  // CNS = 00h: Identify Namespace data structure

    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

int ATASecureEraseAlgorithm::nvme_get_sanitize_log(int fd, uint8_t* out) {
    nvme_admin_cmd cmd{};
    cmd.opcode = 0x02;  // Get Log Page
    cmd.nsid = 0xFFFF'FFFF;
    cmd.addr = reinterpret_cast<uint64_t>(out);
    cmd.data_len = nvme_sanitize::SANITIZE_LOG_SIZE;
    cmd.cdw10 = static_cast<uint32_t>(nvme_sanitize::SANITIZE_LOG_ID) |
                (static_cast<uint32_t>((nvme_sanitize::SANITIZE_LOG_SIZE / 4U) - 1U) << 16U);

    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

bool ATASecureEraseAlgorithm::nvme_format_crypto_erase(int fd, ProgressCallback& callback) {
    const int namespace_id = ioctl(fd, NVME_IOCTL_ID);
    if (namespace_id <= 0) {
        report_progress(callback, 0, "Error", true, true, "Failed to read the NVMe namespace ID");
        return false;
    }

    const auto nsid = static_cast<uint32_t>(namespace_id);

    // Read the namespace's current LBA format so the erase preserves it. A
    // Format that changes the LBA size is a different, riskier operation.
    std::array<uint8_t, 4'096> identify_ns{};
    const int identify_result = nvme_identify_namespace(fd, nsid, identify_ns.data());
    if (identify_result != 0) {
        report_progress(callback, 40, "Error", true, true,
                        std::format("Failed to read Identify Namespace data: {}",
                                    nvme_error_text(identify_result)));
        return false;
    }
    const uint8_t flbas = identify_ns[nvme_sanitize::ID_NS_OFFSET_FLBAS];

    report_progress(callback, 40,
                    "Executing NVMe Format with cryptographic erase (no progress reported "
                    "until the drive finishes)...");

    nvme_admin_cmd cmd{};
    cmd.opcode = 0x80;  // Format NVM
    cmd.nsid = nsid;
    cmd.cdw10 = nvme_sanitize::format_cdw10(flbas, nvme_sanitize::FORMAT_SES_CRYPTO_ERASE);
    cmd.timeout_ms = 3'600'000;  // Format is blocking; allow up to one hour

    const int format_result = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    if (format_result != 0) {
        report_progress(callback, 40, "Error", true, true,
                        std::format("NVMe Format failed: {} (check dmesg for details)",
                                    nvme_error_text(format_result)));
        return false;
    }

    return true;
}

bool ATASecureEraseAlgorithm::nvme_secure_erase(int fd, const FirmwarePlan& firmware,
                                                ProgressCallback& callback,
                                                const std::atomic<bool>& cancel_flag) {
    const auto plan =
        firmware.method == "nvme-sanitize-crypto"  ? nvme_sanitize::ErasePlan::SANITIZE_CRYPTO_ERASE
        : firmware.method == "nvme-sanitize-block" ? nvme_sanitize::ErasePlan::SANITIZE_BLOCK_ERASE
        : firmware.method == "nvme-sanitize-overwrite"
            ? nvme_sanitize::ErasePlan::SANITIZE_OVERWRITE
            : nvme_sanitize::ErasePlan::FORMAT_CRYPTO_ERASE;
    report_progress(callback, 5,
                    std::format("Firmware erase: {} ({})", nvme_sanitize::erase_plan_name(plan),
                                firmware.scope == "nvme-subsystem"
                                    ? "erases all namespaces in the NVMe subsystem"
                                    : "erases only the selected namespace"));

    // Refuse to queue behind an already running sanitize - it would fail
    // anyway, and the log status is how completion is detected below.
    std::array<uint8_t, nvme_sanitize::SANITIZE_LOG_SIZE> log{};
    nvme_sanitize::SanitizeStatus initial_status = nvme_sanitize::SanitizeStatus::UNKNOWN;
    uint32_t initial_cdw10 = 0;
    const int initial_status_result = nvme_get_sanitize_log(fd, log.data());
    if (nvme_sanitize::is_sanitize_plan(plan) && initial_status_result != 0) {
        report_progress(callback, 5, "Error", true, true,
                        "Cannot establish the initial NVMe sanitize status; refusing to start");
        return false;
    }
    if (initial_status_result == 0) {
        const auto initial_page = nvme_sanitize::parse_log_page(log.data());
        initial_status = initial_page.status;
        initial_cdw10 = initial_page.command_dword10;
        if (initial_status == nvme_sanitize::SanitizeStatus::IN_PROGRESS) {
            report_progress(callback, 5, "Error", true, true,
                            "A sanitize operation is already in progress");
            return false;
        }
    }

    if (cancel_flag.load()) {
        report_progress(callback, 5, "Cancelled", true, true, "Operation was cancelled by user");
        return false;
    }

    if (nvme_sanitize::is_sanitize_plan(plan)) {
        report_progress(callback, 10,
                        "Starting NVMe sanitize (it cannot be aborted once "
                        "started)...");

        nvme_admin_cmd cmd{};
        cmd.opcode = 0x84;  // Sanitize
        cmd.addr = 0;
        cmd.data_len = 0;
        // CDW10 carries the action and, for overwrite, the pass count.
        cmd.cdw10 = nvme_sanitize::sanitize_cdw10(plan);
        const auto submitted_cdw10 = cmd.cdw10;
        // CDW11 is OVRPAT, the 32-bit overwrite pattern, and is ignored by the
        // other actions. Zero is a fine pattern; the pass count is in CDW10.
        cmd.cdw11 = 0;

        if (const int result = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd); result != 0) {
            report_progress(callback, 10, "Error", true, true,
                            std::format("Failed to start NVMe sanitize: {} (check dmesg for "
                                        "details)",
                                        nvme_error_text(result)));
            return false;
        }

        // The sanitize runs inside the controller. Poll the Sanitize Status
        // log page until it reports completion.
        const auto start_time = std::chrono::steady_clock::now();
        constexpr auto POLL_INTERVAL = std::chrono::seconds{1};
        constexpr auto MAX_DURATION = std::chrono::hours{24};
        constexpr auto STATUS_READ_RETRIES = 5;
        constexpr auto STATUS_SETTLE_TIME = std::chrono::seconds{5};
        int failed_reads = 0;
        int last_reported_percent = -1;
        int last_percent = 10;
        bool observed_in_progress = false;
        bool cancel_notice_sent = false;

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now - start_time > MAX_DURATION) {
                report_progress(callback, 10, "Error", true, true,
                                "NVMe sanitize did not complete within 24 hours");
                return false;
            }

            if (nvme_get_sanitize_log(fd, log.data()) != 0) {
                if (++failed_reads >= STATUS_READ_RETRIES) {
                    report_progress(callback, 10, "Error", true, true,
                                    "Lost access to the NVMe sanitize status log");
                    return false;
                }
                std::this_thread::sleep_for(POLL_INTERVAL);
                continue;
            }
            failed_reads = 0;

            const auto page = nvme_sanitize::parse_log_page(log.data());
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - start_time)
                                     .count();

            switch (page.status) {
                case nvme_sanitize::SanitizeStatus::IN_PROGRESS:
                case nvme_sanitize::SanitizeStatus::NEVER_SANITIZED:
                case nvme_sanitize::SanitizeStatus::UNKNOWN: {
                    // NEVER_SANITIZED can be observed briefly right after the
                    // command is submitted, before the controller updates the
                    // status field - keep polling either way.
                    if (page.status == nvme_sanitize::SanitizeStatus::IN_PROGRESS) {
                        observed_in_progress = true;
                        const double percent =
                            10.0 + (static_cast<double>(page.progress_percent) * 0.88);
                        last_percent = static_cast<int>(percent);
                        if (last_percent != last_reported_percent) {
                            last_reported_percent = last_percent;
                            std::string status_text = std::format("NVMe sanitize in progress: {}%",
                                                                  page.progress_percent);
                            if (page.progress_percent > 5) {
                                const int64_t eta =
                                    elapsed * (100 - page.progress_percent) / page.progress_percent;
                                status_text += std::format(" - ETA: {}", format_duration(eta));
                            }
                            report_progress(callback, percent, status_text);
                        }
                    }

                    if (cancel_flag.load() && !cancel_notice_sent) {
                        // The command cannot be aborted; keep going rather
                        // than abandon the wipe mid-erase. Said once, at the
                        // percentage reached, so the bar does not jump back.
                        cancel_notice_sent = true;
                        report_progress(callback, last_percent,
                                        "Cancellation requested, but an NVMe sanitize cannot "
                                        "be aborted - continuing to completion");
                    }
                    break;
                }
                case nvme_sanitize::SanitizeStatus::COMPLETED_SUCCESS:
                case nvme_sanitize::SanitizeStatus::COMPLETED_SUCCESS_NO_DEALLOCATE: {
                    // A controller that was sanitized before still reports the
                    // previous run's terminal status until it records this
                    // command. Bind the verdict to SCDW10 (or an observed
                    // in-progress transition) instead of trusting a stale
                    // success. If the status never identifies this command,
                    // fail closed rather than certify an erase that may not
                    // have happened.
                    const bool recorded = page.command_dword10 == submitted_cdw10;
                    const bool command_changed = page.command_dword10 != initial_cdw10;
                    const bool belongs_to_this_run = observed_in_progress ||
                                                     page.status != initial_status ||
                                                     (recorded && command_changed);
                    if (!belongs_to_this_run) {
                        if (now - start_time < STATUS_SETTLE_TIME) {
                            break;
                        }
                        report_progress(callback, 10, "Error", true, true,
                                        "The controller did not record this sanitize command; "
                                        "cannot certify completion. Check dmesg and the device "
                                        "state before retrying.");
                        return false;
                    }

                    std::string note =
                        page.status ==
                                nvme_sanitize::SanitizeStatus::COMPLETED_SUCCESS_NO_DEALLOCATE
                            ? " (no-deallocate was not honoured by the controller)"
                            : "";
                    report_progress(callback, 100,
                                    std::format("NVMe sanitize completed successfully in {} ({}){}",
                                                format_duration(elapsed),
                                                nvme_sanitize::erase_plan_name(plan), note),
                                    true, false, "");
                    return true;
                }
                case nvme_sanitize::SanitizeStatus::COMPLETED_FAILED: {
                    // A failure left by a previous run must not stop
                    // monitoring a command this controller has not recorded.
                    const bool recorded = page.command_dword10 == submitted_cdw10;
                    const bool command_changed = page.command_dword10 != initial_cdw10;
                    if (!observed_in_progress && page.status == initial_status &&
                        !(recorded && command_changed)) {
                        if (now - start_time < STATUS_SETTLE_TIME) {
                            break;
                        }
                        report_progress(callback, 10, "Error", true, true,
                                        "The controller did not record this sanitize command; "
                                        "cannot certify completion. Check dmesg and the device "
                                        "state before retrying.");
                        return false;
                    }
                    report_progress(callback, 10, "Error", true, true,
                                    "The NVMe sanitize operation failed inside the controller "
                                    "(check dmesg for details)");
                    return false;
                }
            }

            std::this_thread::sleep_for(POLL_INTERVAL);
        }
    }

    // FORMAT_CRYPTO_ERASE: blocking command with no progress reporting
    if (nvme_format_crypto_erase(fd, callback)) {
        report_progress(callback, 100,
                        "NVMe Format with cryptographic erase completed successfully", true, false,
                        "");
        return true;
    }
    return false;
}

std::string ATASecureEraseAlgorithm::format_duration(int64_t seconds) {
    if (seconds < 0) {
        return "unknown";
    }
    const auto hours = seconds / 3'600;
    const auto minutes = (seconds % 3'600) / 60;
    const auto secs = seconds % 60;
    if (hours > 0) {
        return std::format("{}h {:02d}m", hours, minutes);
    }
    if (minutes > 0) {
        return std::format("{}m {:02d}s", minutes, secs);
    }
    return std::format("{}s", secs);
}
