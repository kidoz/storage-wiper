/**
 * @file ATASecureEraseAlgorithm.cpp
 * @brief Implementation of ATA Secure Erase using Linux ioctl
 */

#include "algorithms/ATASecureEraseAlgorithm.hpp"

#include "models/WipeTypes.hpp"

#include <fcntl.h>
#include <linux/hdreg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <scsi/sg.h>

// ATA pass-through command structure for SG_IO
struct ata_passthrough_cmd {
    uint8_t opcode;    // 0x85 for ATA PASS-THROUGH (16)
    uint8_t protocol;  // Protocol field
    uint8_t flags;     // t_length, t_dir, t_type, byt_block
    uint8_t features_high;
    uint8_t features_low;
    uint8_t sector_count_high;
    uint8_t sector_count_low;
    uint8_t lba_low_high;
    uint8_t lba_low_low;
    uint8_t lba_mid_high;
    uint8_t lba_mid_low;
    uint8_t lba_high_high;
    uint8_t lba_high_low;
    uint8_t device;
    uint8_t command;
    uint8_t control;
};

bool ATASecureEraseAlgorithm::execute([[maybe_unused]] int fd, [[maybe_unused]] uint64_t size,
                                      ProgressCallback callback,
                                      [[maybe_unused]] const std::atomic<bool>& cancel_flag) {
    // This method should not be called for ATA Secure Erase
    // Use execute_on_device instead
    report_progress(callback, 0, "Error: Use execute_on_device for ATA Secure Erase", true, true,
                    "ATA Secure Erase requires device path, not file descriptor");
    return false;
}

bool ATASecureEraseAlgorithm::execute_on_device(const std::string& device_path,
                                                [[maybe_unused]] uint64_t size,
                                                ProgressCallback callback,
                                                const std::atomic<bool>& cancel_flag) {
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
                        "This may be a USB device, NVMe drive, or older hardware. "
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
        report_progress(callback, 0, "Cancelled", true, false, "");
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
        report_progress(callback, 5, "Cancelled - password disabled", true, false, "");
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
        report_progress(callback, 10, "Cancelled", true, false, "");
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

bool ATASecureEraseAlgorithm::send_ata_command(int fd, uint8_t command, const void* data,
                                               size_t data_size, bool data_out) {
    // Use HDIO_DRIVE_CMD for simple commands
    uint8_t cmd_buf[4 + 512];
    std::memset(cmd_buf, 0, sizeof(cmd_buf));

    cmd_buf[0] = command;
    cmd_buf[1] = 0;  // sector count (for commands that need it)
    cmd_buf[2] = 0;  // features
    cmd_buf[3] = 0;  // sector number

    if (data_out && data && data_size > 0) {
        size_t copy_size = std::min(data_size, size_t(512));
        std::memcpy(&cmd_buf[4], data, copy_size);
    }

    if (ioctl(fd, HDIO_DRIVE_CMD, cmd_buf) != 0) {
        return false;
    }

    return true;
}

bool ATASecureEraseAlgorithm::read_identify_data(int fd, uint16_t* identify_data) {
    struct hd_driveid* drive_id = reinterpret_cast<struct hd_driveid*>(identify_data);
    return ioctl(fd, HDIO_GET_IDENTITY, drive_id) == 0;
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
