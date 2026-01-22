/**
 * @file VSITRAlgorithm.hpp
 * @brief German BSI VSITR 7-pass wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"

/**
 * @class VSITRAlgorithm
 * @brief German BSI VSITR 7-pass standard
 */
class VSITRAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                 const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override { return "VSITR"; }

    std::string get_description() const override { return "German BSI VSITR 7-pass standard"; }

    int get_pass_count() const override { return 7; }

    bool is_ssd_compatible() const override { return false; }

private:
    static constexpr size_t BUFFER_SIZE = 1'024 * 1'024;  // 1MB buffer

    bool write_pattern(int fd, uint64_t size, const uint8_t* pattern, size_t pattern_size,
                       ProgressCallback callback, int pass, int total_passes,
                       const std::atomic<bool>& cancel_flag);
};
