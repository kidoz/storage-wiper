/**
 * @file ATASecureEraseAlgorithm.hpp
 * @brief ATA Secure Erase hardware-based wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"
#include <string>

/**
 * @class ATASecureEraseAlgorithm
 * @brief Hardware-based secure erase for SSDs
 *
 * Note: This is a placeholder implementation. Full implementation would
 * require proper ATA command support via ioctl instead of system calls.
 */
class ATASecureEraseAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override {
        return "ATA Secure Erase";
    }

    std::string get_description() const override {
        return "Hardware-based secure erase for SSDs";
    }

    int get_pass_count() const override {
        return 1;
    }

    bool is_ssd_compatible() const override {
        return true;
    }
};
