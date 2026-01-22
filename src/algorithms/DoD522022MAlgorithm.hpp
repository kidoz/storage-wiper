/**
 * @file DoD522022MAlgorithm.hpp
 * @brief US Department of Defense 5220.22-M wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"

/**
 * @class DoD522022MAlgorithm
 * @brief DoD 5220.22-M 3-pass standard algorithm
 */
class DoD522022MAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                 const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override { return "DoD 5220.22-M"; }

    std::string get_description() const override {
        return "US Department of Defense 3-pass standard";
    }

    int get_pass_count() const override { return 3; }

    bool is_ssd_compatible() const override { return false; }

    bool supports_verification() const override { return true; }

    bool verify(int fd, uint64_t size, ProgressCallback callback,
               const std::atomic<bool>& cancel_flag) override;

private:
    static constexpr size_t BUFFER_SIZE = 1'024 * 1'024;  // 1MB buffer

    bool write_pattern(int fd, uint64_t size, const uint8_t* pattern, size_t pattern_size,
                       ProgressCallback callback, int pass, int total_passes,
                       const std::atomic<bool>& cancel_flag);
};
