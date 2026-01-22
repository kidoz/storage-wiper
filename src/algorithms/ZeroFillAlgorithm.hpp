/**
 * @file ZeroFillAlgorithm.hpp
 * @brief Zero fill wipe algorithm implementation
 */

#pragma once

#include "IWipeAlgorithm.hpp"

#include <vector>

/**
 * @class ZeroFillAlgorithm
 * @brief Single-pass zero fill algorithm
 */
class ZeroFillAlgorithm : public IWipeAlgorithm {
public:
    bool execute(int fd, uint64_t size, ProgressCallback callback,
                 const std::atomic<bool>& cancel_flag) override;

    std::string get_name() const override { return "Zero Fill"; }

    std::string get_description() const override { return "Single pass overwrite with zeros"; }

    int get_pass_count() const override { return 1; }

    bool is_ssd_compatible() const override { return true; }

private:
    static constexpr size_t BUFFER_SIZE = 1'024 * 1'024;  // 1MB buffer
};
