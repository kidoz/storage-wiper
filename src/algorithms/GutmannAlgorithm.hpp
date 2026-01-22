/**
 * @file GutmannAlgorithm.hpp
 * @brief Peter Gutmann's 35-pass wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"

/**
 * @class GutmannAlgorithm
 * @brief Peter Gutmann's 35-pass secure deletion method
 */
class GutmannAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                 const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override { return "Gutmann"; }

    std::string get_description() const override {
        return "Peter Gutmann's 35-pass secure deletion";
    }

    int get_pass_count() const override { return 35; }

    bool is_ssd_compatible() const override { return false; }

private:
    static constexpr size_t BUFFER_SIZE = 1'024 * 1'024;  // 1MB buffer

    bool write_pattern(int fd, uint64_t size, const uint8_t* pattern, size_t pattern_size,
                       ProgressCallback callback, int pass, int total_passes,
                       const std::atomic<bool>& cancel_flag);
};
