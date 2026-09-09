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
    // NVMe drives take the Sanitize/Format path through the controller
    if (device_path.starts_with("/dev/nvme")) {
        return nvme_secure_erase(device_path, callback, cancel_flag);
    }

    report_progress(callback, 0, "Checking ATA Security support...");

    // Open device
    int fd = open(device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::string error = "Failed to open device: " + std::string(strerror(errno));
        report_progress(callback, 0, "Error", true, true, error);
        return false;
    }

    // Get security info
    ATASecurityInfo security_info = get_security_info(device_path);

    if (!security_info.supported) {
        close(fd);
        report_progress(callback, 0, "Error", true, true,
                        "Device does not support ATA Security feature. "
                        "This may be a USB device or older hardware. "
                        "Consider using Zero Fill or Random Data instead.");
        return false;
    }

    if (security_info.frozen) {
        close(fd);
        report_progress(callback, 0, "Error", true, true,
                        "Device security is frozen. To unfreeze:\n"
                        "1. Suspend the system (sleep)\n"
                        "2. Wake it up\n"
                        "3. Run secure erase immediately\n\n"
                        "Alternatively, a cold boot without BIOS freeze may work.");
        return false;
    }

    if (security_info.locked) {
        close(fd);
        report_progress(callback, 0, "Error", true, true,
                        "Device is locked with a security password. "
                        "You must unlock it first with the correct password.");
        return false;
    }

    if (security_info.count_expired) {
        close(fd);
        report_progress(callback, 0, "Error", true, true,
                        "Security attempt count expired. "
                        "The device has been locked due to too many failed attempts.");
        return false;
    }

    // Check for cancellation
    if (cancel_flag.load()) {
        close(fd);
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
        close(fd);
        report_progress(callback, 5, "Error", true, true,
                        "Failed to set security password. The device may not accept "
                        "password commands or may require specific conditions.");
        return false;
    }

    if (cancel_flag.load()) {
        // Try to disable password before exiting
        disable_security_password(fd, TEMP_PASSWORD, false);
        close(fd);
        report_progress(callback, 5, "Cancelled - password disabled", true, true,
                        "Operation was cancelled by user");
        return false;
    }

    report_progress(callback, 10, "Preparing for secure erase...");

    // Step 2: Security erase prepare
    if (!security_erase_prepare(fd)) {
        // Try to disable password
        disable_security_password(fd, TEMP_PASSWORD, false);
        close(fd);
        report_progress(callback, 10, "Error", true, true,
                        "Failed to prepare for security erase. "
                        "The device rejected the SECURITY ERASE PREPARE command.");
        return false;
    }

    if (cancel_flag.load()) {
        disable_security_password(fd, TEMP_PASSWORD, false);
        close(fd);
        report_progress(callback, 10, "Cancelled", true, true, "Operation was cancelled by user");
        return false;
    }

    report_progress(callback, 15, "Starting ATA Secure Erase" + time_msg);

    // Step 3: Execute secure erase
    // This command can take a very long time (minutes to hours)
    bool use_enhanced = security_info.enhanced_erase_supported;
    auto start_time = std::chrono::steady_clock::now();

    if (!security_erase_unit(fd, TEMP_PASSWORD, use_enhanced, false)) {
        // The erase may have failed but password should be cleared on success
        // Try to disable password in case it's still set
        disable_security_password(fd, TEMP_PASSWORD, false);
        close(fd);
        report_progress(callback, 15, "Error", true, true,
                        "Secure erase command failed. The device may have:\n"
                        "- Timed out (erase takes too long)\n"
                        "- Rejected the command\n"
                        "- Encountered a hardware error\n\n"
                        "Check dmesg for more information.");
        return false;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    // Verify the erase completed and password was cleared
    close(fd);

    // Reopen to check status
    fd = open(device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd >= 0) {
        ATASecurityInfo post_info = get_security_info(device_path);
        close(fd);

        if (post_info.enabled) {
            // Password still set - try to disable it
            fd = open(device_path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd >= 0) {
                disable_security_password(fd, TEMP_PASSWORD, false);
                close(fd);
            }
        }
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
    ATASecurityInfo info{};

    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return info;
    }

    // Try to get IDENTIFY data using hdparm-style ioctl
    uint16_t identify_data[256];
    std::memset(identify_data, 0, sizeof(identify_data));

    // Use HDIO_GET_IDENTITY ioctl
    struct hd_driveid* drive_id = reinterpret_cast<struct hd_driveid*>(identify_data);
    if (ioctl(fd, HDIO_GET_IDENTITY, drive_id) == 0) {
        // Parse security word (word 128)
        uint16_t security_word = identify_data[SECURITY_WORD];

        info.supported = (security_word & SECURITY_SUPPORTED) != 0;
        info.enabled = (security_word & SECURITY_ENABLED) != 0;
        info.locked = (security_word & SECURITY_LOCKED) != 0;
        info.frozen = (security_word & SECURITY_FROZEN) != 0;
        info.count_expired = (security_word & SECURITY_COUNT_EXPIRED) != 0;
        info.enhanced_erase_supported = (security_word & SECURITY_ENHANCED_ERASE) != 0;

        // Get erase times (words 89 and 90)
        info.erase_time_normal = identify_data[ERASE_TIME_WORD];
        info.erase_time_enhanced = identify_data[ENHANCED_ERASE_TIME_WORD];
        info.master_password_revision = identify_data[MASTER_PASSWORD_REV_WORD] & 0xFF;

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

    close(fd);
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
    buffer[0] = control & 0xFF;
    buffer[1] = (control >> 8) & 0xFF;

    // Copy password (max 32 bytes)
    size_t pwd_len = std::strlen(password);
    size_t copy_len = std::min(pwd_len, size_t(32));
    std::memcpy(&buffer[2], password, copy_len);

    // Using HDIO_DRIVE_CMD approach with data transfer
    uint8_t cmd_data[4 + 512];
    std::memset(cmd_data, 0, sizeof(cmd_data));
    cmd_data[0] = ATA_OP_SECURITY_SET_PASSWORD;
    cmd_data[1] = 1;  // sector count
    cmd_data[2] = 0;  // features
    cmd_data[3] = 1;  // nsect for data transfer
    std::memcpy(&cmd_data[4], buffer, 512);

    // HDIO_DRIVE_CMD_AES for data transfer commands
    // Actually, HDIO_DRIVE_CMD expects:
    // [0] = command, [1] = nsect, [2] = feature, [3] = nsect
    // Then 512 bytes of data

    if (ioctl(fd, HDIO_DRIVE_CMD, cmd_data) != 0) {
        // Try alternate method
        return false;
    }

    return true;
}

bool ATASecureEraseAlgorithm::disable_security_password(int fd, const char* password, bool master) {
    uint8_t buffer[512];
    std::memset(buffer, 0, sizeof(buffer));

    uint16_t control = master ? 0x0001 : 0x0000;
    buffer[0] = control & 0xFF;
    buffer[1] = (control >> 8) & 0xFF;

    size_t pwd_len = std::strlen(password);
    size_t copy_len = std::min(pwd_len, size_t(32));
    std::memcpy(&buffer[2], password, copy_len);

    uint8_t cmd_data[4 + 512];
    std::memset(cmd_data, 0, sizeof(cmd_data));
    cmd_data[0] = ATA_OP_SECURITY_DISABLE_PASSWORD;
    cmd_data[1] = 1;
    cmd_data[2] = 0;
    cmd_data[3] = 1;
    std::memcpy(&cmd_data[4], buffer, 512);

    return ioctl(fd, HDIO_DRIVE_CMD, cmd_data) == 0;
}

bool ATASecureEraseAlgorithm::security_erase_prepare(int fd) {
    uint8_t args[4];
    std::memset(args, 0, sizeof(args));
    args[0] = ATA_OP_SECURITY_ERASE_PREPARE;

    return ioctl(fd, HDIO_DRIVE_CMD, args) == 0;
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
        control |= 0x0001;
    if (enhanced)
        control |= 0x0002;

    buffer[0] = control & 0xFF;
    buffer[1] = (control >> 8) & 0xFF;

    size_t pwd_len = std::strlen(password);
    size_t copy_len = std::min(pwd_len, size_t(32));
    std::memcpy(&buffer[2], password, copy_len);

    uint8_t cmd_data[4 + 512];
    std::memset(cmd_data, 0, sizeof(cmd_data));
    cmd_data[0] = ATA_OP_SECURITY_ERASE_UNIT;
    cmd_data[1] = 1;
    cmd_data[2] = 0;
    cmd_data[3] = 1;
    std::memcpy(&cmd_data[4], buffer, 512);

    // This command can take a very long time
    // The ioctl may timeout - need to use appropriate timeout or async handling
    return ioctl(fd, HDIO_DRIVE_CMD, cmd_data) == 0;
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
    cmd.cdw10 = nvme_sanitize::SANITIZE_LOG_ID |
                (static_cast<uint32_t>((nvme_sanitize::SANITIZE_LOG_SIZE / 4) - 1) << 16);

    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

bool ATASecureEraseAlgorithm::nvme_format_crypto_erase(const std::string& device_path, int ctrl_fd,
                                                       ProgressCallback& callback) {
    // Format NVM is namespaced, so it needs the namespace ID of the device
    // being wiped. Sanitize by contrast is controller-wide and takes none.
    const util::FileDescriptor ns_fd(open(device_path.c_str(), O_RDONLY | O_NONBLOCK));
    if (!ns_fd) {
        report_progress(callback, 0, "Error", true, true,
                        "Failed to open NVMe namespace to read its namespace ID: " +
                            std::string(strerror(errno)));
        return false;
    }

    const int namespace_id = ioctl(ns_fd.get(), NVME_IOCTL_ID);
    if (namespace_id <= 0) {
        report_progress(callback, 0, "Error", true, true,
                        "Failed to read the NVMe namespace ID of " + device_path);
        return false;
    }

    const auto nsid = static_cast<uint32_t>(namespace_id);

    // Read the namespace's current LBA format so the erase preserves it. A
    // Format that changes the LBA size is a different, riskier operation.
    std::array<uint8_t, 4'096> identify_ns{};
    const int identify_result = nvme_identify_namespace(ctrl_fd, nsid, identify_ns.data());
    if (identify_result != 0) {
        report_progress(callback, 40, "Error", true, true,
                        std::format("Failed to read Identify Namespace data for {}: {}",
                                    device_path, nvme_error_text(identify_result)));
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

    const int format_result = ioctl(ctrl_fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    if (format_result != 0) {
        report_progress(callback, 40, "Error", true, true,
                        std::format("NVMe Format failed: {} (check dmesg for details)",
                                    nvme_error_text(format_result)));
        return false;
    }

    return true;
}

bool ATASecureEraseAlgorithm::nvme_secure_erase(const std::string& device_path,
                                                ProgressCallback& callback,
                                                const std::atomic<bool>& cancel_flag) {
    if (cancel_flag.load()) {
        report_progress(callback, 0, "Cancelled", true, true, "Operation was cancelled by user");
        return false;
    }

    const auto ctrl_path = nvme_sanitize::controller_path_of(device_path);
    if (!ctrl_path) {
        report_progress(callback, 0, "Error", true, true,
                        "Cannot determine the NVMe controller device for " + device_path);
        return false;
    }

    const util::FileDescriptor ctrl_fd(open(ctrl_path->c_str(), O_RDWR | O_NONBLOCK));
    if (!ctrl_fd) {
        report_progress(callback, 0, "Error", true, true,
                        "Failed to open NVMe controller " + *ctrl_path + ": " +
                            std::string(strerror(errno)));
        return false;
    }

    report_progress(callback, 0, "Reading NVMe controller capabilities...");

    std::array<uint8_t, 4'096> identify{};
    if (const int result = nvme_identify_controller(ctrl_fd.get(), identify.data()); result != 0) {
        report_progress(callback, 0, "Error", true, true,
                        "Failed to read Identify Controller data from " + *ctrl_path + ": " +
                            nvme_error_text(result));
        return false;
    }

    const auto caps = nvme_sanitize::parse_capabilities(identify.data());
    const auto plan = nvme_sanitize::choose_erase_plan(caps);
    if (plan == nvme_sanitize::ErasePlan::UNSUPPORTED) {
        report_progress(callback, 0, "Error", true, true,
                        "This NVMe controller supports neither the Sanitize command nor "
                        "cryptographic Format NVM. Use a software overwrite algorithm "
                        "(Zero Fill or Random Data) instead.");
        return false;
    }

    report_progress(callback, 5,
                    std::format("Firmware erase: {} (erases all namespaces on {})",
                                nvme_sanitize::erase_plan_name(plan), *ctrl_path));

    // Refuse to queue behind an already running sanitize - it would fail
    // anyway, and the log status is how completion is detected below.
    std::array<uint8_t, nvme_sanitize::SANITIZE_LOG_SIZE> log{};
    bool prior_run_succeeded = false;
    if (nvme_get_sanitize_log(ctrl_fd.get(), log.data()) == 0) {
        const auto status = nvme_sanitize::parse_log_page(log.data()).status;
        if (status == nvme_sanitize::SanitizeStatus::IN_PROGRESS) {
            report_progress(callback, 5, "Error", true, true,
                            "A sanitize operation is already in progress on " + *ctrl_path);
            return false;
        }
        // Remembered so a success left over from an earlier sanitize is not
        // mistaken for this one completing instantly.
        prior_run_succeeded = nvme_sanitize::is_success_status(status);
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
        // CDW11 is OVRPAT, the 32-bit overwrite pattern, and is ignored by the
        // other actions. Zero is a fine pattern; the pass count is in CDW10.
        cmd.cdw11 = 0;

        if (const int result = ioctl(ctrl_fd.get(), NVME_IOCTL_ADMIN_CMD, &cmd); result != 0) {
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

            if (nvme_get_sanitize_log(ctrl_fd.get(), log.data()) != 0) {
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
                    // A drive sanitized before still reports the previous
                    // run's success until the controller updates the log. If
                    // this erase was never seen running, give the status time
                    // to settle rather than certify an erase that may not have
                    // happened.
                    if (prior_run_succeeded && !observed_in_progress &&
                        now - start_time < STATUS_SETTLE_TIME) {
                        break;
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
    if (nvme_format_crypto_erase(device_path, ctrl_fd.get(), callback)) {
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
