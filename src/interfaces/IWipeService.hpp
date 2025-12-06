/**
 * @file IWipeService.hpp
 * @brief Interface for secure disk wiping operations
 * 
 * This file defines the interface for secure disk wiping with multiple
 * DoD-compliant algorithms and progress reporting.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

/**
 * @enum WipeAlgorithm
 * @brief Available disk wiping algorithms
 */
enum class WipeAlgorithm {
    ZERO_FILL,           ///< Single pass with zeros
    RANDOM_FILL,         ///< Single pass with random data
    DOD_5220_22_M,      ///< DoD 5220.22-M 3-pass standard
    GUTMANN,            ///< Gutmann 35-pass method
    SCHNEIER,           ///< Bruce Schneier 7-pass method
    VSITR,              ///< German VSITR 7-pass standard
    GOST_R_50739_95,    ///< Russian GOST R 50739-95 2-pass standard
    ATA_SECURE_ERASE    ///< Hardware secure erase for SSDs
};

struct WipeProgress {
    uint64_t bytes_written = 0;
    uint64_t total_bytes = 0;
    int current_pass = 0;
    int total_passes = 0;
    double percentage = 0.0;
    std::string status;
    bool is_complete = false;
    bool has_error = false;
    std::string error_message;

    auto operator==(const WipeProgress&) const -> bool = default;
};

using ProgressCallback = std::function<void(const WipeProgress&)>;

class IWipeService {
public:
    virtual ~IWipeService() = default;

    virtual auto wipe_disk(const std::string& disk_path,
                          WipeAlgorithm algorithm,
                          ProgressCallback callback) -> bool = 0;

    [[nodiscard]] virtual auto get_algorithm_name(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_algorithm_description(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_pass_count(WipeAlgorithm algo) -> int = 0;
    [[nodiscard]] virtual auto is_ssd_compatible(WipeAlgorithm algo) -> bool = 0;
    virtual auto cancel_current_operation() -> bool = 0;
};