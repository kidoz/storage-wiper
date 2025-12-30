/**
 * @file ATASecureEraseAlgorithm.hpp
 * @brief ATA Secure Erase hardware-based wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"
#include <string>
#include <cstdint>

/**
 * @enum ATASecurityState
 * @brief ATA Security feature state
 */
enum class ATASecurityState {
    NOT_SUPPORTED,      ///< Security feature not supported
    DISABLED,           ///< Security not enabled
    ENABLED_UNLOCKED,   ///< Security enabled, device unlocked
    ENABLED_LOCKED,     ///< Security enabled, device locked
    FROZEN,             ///< Security frozen (cannot enable/disable)
    EXPIRED             ///< Security count expired
};

/**
 * @struct ATASecurityInfo
 * @brief Information about ATA Security feature support
 */
struct ATASecurityInfo {
    bool supported = false;
    bool enabled = false;
    bool locked = false;
    bool frozen = false;
    bool count_expired = false;
    bool enhanced_erase_supported = false;
    uint16_t erase_time_normal = 0;      ///< Normal erase time in 2-minute units
    uint16_t erase_time_enhanced = 0;    ///< Enhanced erase time in 2-minute units
    uint8_t master_password_revision = 0;
    ATASecurityState state = ATASecurityState::NOT_SUPPORTED;
};

/**
 * @class ATASecureEraseAlgorithm
 * @brief Hardware-based secure erase for ATA/SATA drives
 *
 * Implements ATA Security feature set commands to perform hardware-level
 * secure erase. This is the most effective way to securely wipe SSDs as
 * it erases all data including wear-leveled blocks.
 *
 * The process:
 * 1. Check if device supports ATA Security
 * 2. Verify device is not frozen
 * 3. Set a temporary password
 * 4. Issue SECURITY ERASE PREPARE
 * 5. Issue SECURITY ERASE UNIT
 * 6. Wait for completion (can take minutes to hours)
 */
class ATASecureEraseAlgorithm : public IWipeAlgorithm {
public:
    /**
     * @brief Not used - ATA Secure Erase requires device-level access
     */
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                const std::atomic<bool>& cancel_flag) override;

    /**
     * @brief Execute ATA Secure Erase on the specified device
     */
    bool execute_on_device(const std::string& device_path, uint64_t size,
                          ProgressCallback callback,
                          const std::atomic<bool>& cancel_flag) override;

    /**
     * @brief ATA Secure Erase requires device-level access
     */
    bool requires_device_access() const override { return true; }

    std::string get_name() const override {
        return "ATA Secure Erase";
    }

    std::string get_description() const override {
        return "Hardware-based secure erase using ATA Security commands. "
               "Most effective for SSDs - erases all blocks including wear-leveled areas.";
    }

    int get_pass_count() const override {
        return 1;
    }

    bool is_ssd_compatible() const override {
        return true;
    }

    /**
     * @brief Check if a device supports ATA Secure Erase
     * @param device_path Path to the device
     * @return ATASecurityInfo with security feature information
     */
    static ATASecurityInfo get_security_info(const std::string& device_path);

    /**
     * @brief Check if the device is frozen
     * @param device_path Path to the device
     * @return true if device is frozen
     */
    static bool is_device_frozen(const std::string& device_path);

private:
    // ATA command codes
    static constexpr uint8_t ATA_OP_IDENTIFY = 0xEC;
    static constexpr uint8_t ATA_OP_SECURITY_SET_PASSWORD = 0xF1;
    static constexpr uint8_t ATA_OP_SECURITY_UNLOCK = 0xF2;
    static constexpr uint8_t ATA_OP_SECURITY_ERASE_PREPARE = 0xF3;
    static constexpr uint8_t ATA_OP_SECURITY_ERASE_UNIT = 0xF4;
    static constexpr uint8_t ATA_OP_SECURITY_FREEZE_LOCK = 0xF5;
    static constexpr uint8_t ATA_OP_SECURITY_DISABLE_PASSWORD = 0xF6;

    // Security word offsets in IDENTIFY data
    static constexpr int SECURITY_WORD = 128;
    static constexpr int ERASE_TIME_WORD = 89;
    static constexpr int ENHANCED_ERASE_TIME_WORD = 90;
    static constexpr int MASTER_PASSWORD_REV_WORD = 92;

    // Security word bit masks
    static constexpr uint16_t SECURITY_SUPPORTED = 0x0001;
    static constexpr uint16_t SECURITY_ENABLED = 0x0002;
    static constexpr uint16_t SECURITY_LOCKED = 0x0004;
    static constexpr uint16_t SECURITY_FROZEN = 0x0008;
    static constexpr uint16_t SECURITY_COUNT_EXPIRED = 0x0010;
    static constexpr uint16_t SECURITY_ENHANCED_ERASE = 0x0020;

    // Temporary password for secure erase
    static constexpr char TEMP_PASSWORD[] = "StorageWiper";

    /**
     * @brief Send ATA command via ioctl
     */
    bool send_ata_command(int fd, uint8_t command, const void* data = nullptr,
                         size_t data_size = 0, bool data_out = false);

    /**
     * @brief Read IDENTIFY DEVICE data
     */
    bool read_identify_data(int fd, uint16_t* identify_data);

    /**
     * @brief Set ATA security password
     */
    bool set_security_password(int fd, const char* password, bool master = false);

    /**
     * @brief Disable ATA security password
     */
    bool disable_security_password(int fd, const char* password, bool master = false);

    /**
     * @brief Prepare for security erase
     */
    bool security_erase_prepare(int fd);

    /**
     * @brief Execute security erase unit command
     */
    bool security_erase_unit(int fd, const char* password, bool enhanced = false,
                            bool master = false);

    /**
     * @brief Report progress to callback
     */
    void report_progress(ProgressCallback& callback, double percentage,
                        const std::string& status, bool complete = false,
                        bool error = false, const std::string& error_msg = "");
};
