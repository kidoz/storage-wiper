/**
 * @file VerificationHelper.hpp
 * @brief Utility functions for verifying wipe operations
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <atomic>
#include <cstdint>

namespace verification {

/**
 * @brief Verify that a device contains all zeros
 * @param fd File descriptor (opened for reading)
 * @param size Device size in bytes
 * @param callback Progress callback
 * @param cancel_flag Cancellation flag
 * @return true if all bytes are zero
 */
[[nodiscard]] auto verify_zeros(int fd, uint64_t size, ProgressCallback callback,
                                const std::atomic<bool>& cancel_flag) -> bool;

/**
 * @brief Verify that a device contains a repeating byte pattern
 * @param fd File descriptor (opened for reading)
 * @param size Device size in bytes
 * @param pattern Expected byte value
 * @param callback Progress callback
 * @param cancel_flag Cancellation flag
 * @return true if all bytes match the pattern
 */
[[nodiscard]] auto verify_pattern(int fd, uint64_t size, uint8_t pattern, ProgressCallback callback,
                                  const std::atomic<bool>& cancel_flag) -> bool;

/**
 * @brief Verify that a device plausibly contains CSPRNG output
 *
 * Reads the whole device, accumulates a byte-value histogram, and applies a
 * chi-squared uniformity test (255 degrees of freedom, p = 0.001). Genuine
 * CSPRNG output scores ~255 regardless of device size; residual structured
 * data (filesystems, text, zeros) scores far above the critical value.
 *
 * @param fd File descriptor (opened for reading)
 * @param size Device size in bytes
 * @param callback Progress callback
 * @param cancel_flag Cancellation flag
 * @return true if the byte distribution is statistically uniform
 */
[[nodiscard]] auto verify_random(int fd, uint64_t size, ProgressCallback callback,
                                 const std::atomic<bool>& cancel_flag) -> bool;

}  // namespace verification
