/**
 * @file WipeTypes.hpp
 * @brief Data types for disk wiping operations
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
    ZERO_FILL,        ///< Single pass with zeros
    RANDOM_FILL,      ///< Single pass with random data
    DOD_5220_22_M,    ///< DoD 5220.22-M 3-pass standard
    GUTMANN,          ///< Gutmann 35-pass method
    SCHNEIER,         ///< Bruce Schneier 7-pass method
    VSITR,            ///< German VSITR 7-pass standard
    GOST_R_50739_95,  ///< Russian GOST R 50739-95 2-pass standard
    ATA_SECURE_ERASE  ///< Hardware secure erase for SSDs
};

/**
 * @struct WipeProgress
 * @brief Progress information for wipe operations
 */
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
    uint64_t speed_bytes_per_sec = 0;          ///< Current write speed in bytes/sec
    int64_t estimated_seconds_remaining = -1;  ///< ETA in seconds, -1 if unknown

    // Verification fields
    bool verification_enabled = false;         ///< Whether verification was requested
    bool verification_in_progress = false;     ///< Currently verifying (not wiping)
    bool verification_passed = false;          ///< Verification result (only valid when complete)
    double verification_percentage = 0.0;      ///< Verification progress (0-100)
    uint64_t verification_mismatches = 0;      ///< Number of bytes that didn't match

    auto operator==(const WipeProgress&) const -> bool = default;
};

/**
 * @brief Callback type for progress reporting
 */
using ProgressCallback = std::function<void(const WipeProgress&)>;
