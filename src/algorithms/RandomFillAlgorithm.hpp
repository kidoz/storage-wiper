/**
 * @file RandomFillAlgorithm.hpp
 * @brief Random data fill wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"

/**
 * @class RandomFillAlgorithm
 * @brief Single-pass random data fill algorithm
 */
class RandomFillAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override {
        return "Random Data";
    }

    std::string get_description() const override {
        return "Single pass overwrite with random data";
    }

    int get_pass_count() const override {
        return 1;
    }

    bool is_ssd_compatible() const override {
        return true;
    }

private:
    static constexpr size_t BUFFER_SIZE = 1024 * 1024; // 1MB buffer
};
