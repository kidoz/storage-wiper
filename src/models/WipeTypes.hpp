/**
 * @file WipeTypes.hpp
 * @brief Data types for disk wiping operations
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

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
    ATA_SECURE_ERASE  ///< Hardware secure erase (ATA Security Erase / NVMe Sanitize)
};

/**
 * @brief NIST SP 800-88 Rev.1 media sanitization category of an algorithm
 *
 * Overwrite-based algorithms correspond to the "Clear" category: they remove
 * data from user-addressable locations only. Firmware/hardware erase
 * (ATA Security Erase, NVMe Sanitize) corresponds to "Purge", which also
 * addresses remapped blocks and wear-leveled areas the host cannot reach.
 *
 * @param algorithm Wipe algorithm
 * @return Human-readable category label, e.g. "NIST 800-88 Clear"
 */
[[nodiscard]] constexpr auto nist_800_88_category(WipeAlgorithm algorithm) -> std::string_view {
    switch (algorithm) {
        case WipeAlgorithm::ATA_SECURE_ERASE:
            return "NIST 800-88 Purge";
        case WipeAlgorithm::ZERO_FILL:
        case WipeAlgorithm::RANDOM_FILL:
        case WipeAlgorithm::DOD_5220_22_M:
        case WipeAlgorithm::GUTMANN:
        case WipeAlgorithm::SCHNEIER:
        case WipeAlgorithm::VSITR:
        case WipeAlgorithm::GOST_R_50739_95:
            return "NIST 800-88 Clear";
    }
    return "NIST 800-88 Clear";
}

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
    bool verification_enabled = false;      ///< Whether verification was requested
    bool verification_in_progress = false;  ///< Currently verifying (not wiping)
    bool verification_passed = false;       ///< Verification result (only valid when complete)
    double verification_percentage = 0.0;   ///< Verification progress (0-100)
    uint64_t verification_mismatches = 0;   ///< Number of bytes that didn't match

    /// Sectors that could not be written in the current pass (bad-sector
    /// tolerance); reported per pass, so the final value is the number of
    /// distinct unwritable sectors
    uint64_t bad_block_count = 0;

    auto operator==(const WipeProgress&) const -> bool = default;
};

/**
 * @brief Callback type for progress reporting
 */
using ProgressCallback = std::function<void(const WipeProgress&)>;
