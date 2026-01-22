/**
 * @file VerificationHelper.hpp
 * @brief Utility functions for verifying wipe operations
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

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
[[nodiscard]] auto verify_pattern(int fd, uint64_t size, uint8_t pattern,
                                  ProgressCallback callback,
                                  const std::atomic<bool>& cancel_flag) -> bool;

/**
 * @brief Statistical verification that data appears random (high entropy)
 * @param fd File descriptor (opened for reading)
 * @param size Device size in bytes
 * @param callback Progress callback
 * @param cancel_flag Cancellation flag
 * @return true if data passes entropy checks
 *
 * Uses chi-squared test on byte distribution. A truly random fill should
 * have roughly equal distribution of all byte values.
 */
[[nodiscard]] auto verify_random(int fd, uint64_t size, ProgressCallback callback,
                                 const std::atomic<bool>& cancel_flag) -> bool;

/**
 * @brief Verify using a pre-generated pattern buffer
 * @param fd File descriptor (opened for reading)
 * @param size Device size in bytes
 * @param expected_pattern Pattern that should repeat
 * @param callback Progress callback
 * @param cancel_flag Cancellation flag
 * @return true if device matches the pattern
 */
[[nodiscard]] auto verify_buffer_pattern(int fd, uint64_t size,
                                         const std::vector<uint8_t>& expected_pattern,
                                         ProgressCallback callback,
                                         const std::atomic<bool>& cancel_flag) -> bool;

}  // namespace verification
