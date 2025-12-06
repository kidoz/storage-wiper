/**
 * @file GOSTAlgorithm.hpp
 * @brief Russian GOST R 50739-95 wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"

/**
 * @class GOSTAlgorithm
 * @brief Russian GOST R 50739-95 2-pass standard
 *
 * This algorithm implements the commonly-cited GOST R 50739-95 data sanitization method:
 * - Pass 1: Overwrite with zeros (0x00)
 * - Pass 2: Overwrite with random data
 *
 * Note: GOST R 50739-95 is a Russian standard for information protection against
 * unauthorized access. The 2-pass method is widely implemented in data sanitization
 * software as the "GOST R 50739-95" standard, though some debate exists about the
 * original specification's exact requirements.
 */
class GOSTAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override {
        return "GOST R 50739-95";
    }

    std::string get_description() const override {
        return "Russian GOST R 50739-95 2-pass standard";
    }

    int get_pass_count() const override {
        return 2;
    }

    bool is_ssd_compatible() const override {
        return false;
    }

private:
    static constexpr size_t BUFFER_SIZE = 1024 * 1024; // 1MB buffer

    bool write_pattern(int fd, uint64_t size, const uint8_t* pattern,
                      size_t pattern_size, ProgressCallback callback,
                      int pass, int total_passes, const std::atomic<bool>& cancel_flag);
};
