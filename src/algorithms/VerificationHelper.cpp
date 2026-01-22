/**
 * @file VerificationHelper.cpp
 * @brief Implementation of verification utilities
 */

#include "algorithms/VerificationHelper.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace verification {

namespace {
constexpr size_t VERIFY_BUFFER_SIZE = 1'024 * 1'024;  // 1MB buffer

/**
 * @brief Read with retry on EINTR
 */
auto read_with_retry(int fd, void* buf, size_t count) -> ssize_t {
    while (true) {
        ssize_t result = ::read(fd, buf, count);
        if (result >= 0 || errno != EINTR) {
            return result;
        }
    }
}

/**
 * @brief Emit verification progress
 */
void emit_progress(ProgressCallback& callback, uint64_t verified, uint64_t total) {
    if (!callback)
        return;

    WipeProgress progress{};
    progress.verification_in_progress = true;
    progress.verification_percentage = (static_cast<double>(verified) / static_cast<double>(total)) * 100.0;
    progress.bytes_written = verified;
    progress.total_bytes = total;
    progress.percentage = progress.verification_percentage;
    progress.status = "Verifying...";
    callback(progress);
}

}  // namespace

auto verify_zeros(int fd, uint64_t size, ProgressCallback callback,
                  const std::atomic<bool>& cancel_flag) -> bool {
    // Verify that device contains all zeros
    return verify_pattern(fd, size, 0x00, std::move(callback), cancel_flag);
}

auto verify_pattern(int fd, uint64_t size, uint8_t pattern,
                    ProgressCallback callback,
                    const std::atomic<bool>& cancel_flag) -> bool {
    if (size == 0)
        return true;

    // Seek to beginning
    if (lseek(fd, 0, SEEK_SET) != 0) {
        return false;
    }

    std::vector<uint8_t> buffer(VERIFY_BUFFER_SIZE);
    uint64_t verified = 0;
    bool mismatch_found = false;

    while (verified < size && !cancel_flag.load()) {
        size_t to_read = std::min(static_cast<uint64_t>(buffer.size()), size - verified);
        ssize_t bytes_read = read_with_retry(fd, buffer.data(), to_read);

        if (bytes_read <= 0) {
            return false;  // Read error
        }

        // Check all bytes match pattern
        for (ssize_t i = 0; i < bytes_read; ++i) {
            if (buffer[static_cast<size_t>(i)] != pattern) {
                mismatch_found = true;
                break;
            }
        }

        if (mismatch_found) {
            break;
        }

        verified += static_cast<uint64_t>(bytes_read);
        emit_progress(callback, verified, size);
    }

    return !mismatch_found && !cancel_flag.load();
}

auto verify_random(int fd, uint64_t size, ProgressCallback callback,
                   const std::atomic<bool>& cancel_flag) -> bool {
    if (size == 0)
        return true;

    // Seek to beginning
    if (lseek(fd, 0, SEEK_SET) != 0) {
        return false;
    }

    // Count byte frequencies for chi-squared test
    std::array<uint64_t, 256> byte_counts{};
    std::vector<uint8_t> buffer(VERIFY_BUFFER_SIZE);
    uint64_t verified = 0;
    uint64_t total_bytes = 0;

    while (verified < size && !cancel_flag.load()) {
        size_t to_read = std::min(static_cast<uint64_t>(buffer.size()), size - verified);
        ssize_t bytes_read = read_with_retry(fd, buffer.data(), to_read);

        if (bytes_read <= 0) {
            return false;  // Read error
        }

        // Count byte frequencies
        for (ssize_t i = 0; i < bytes_read; ++i) {
            byte_counts[buffer[static_cast<size_t>(i)]]++;
            total_bytes++;
        }

        verified += static_cast<uint64_t>(bytes_read);
        emit_progress(callback, verified, size);
    }

    if (cancel_flag.load()) {
        return false;
    }

    // Chi-squared test for uniform distribution
    // Expected count for each byte value in uniform distribution
    double expected = static_cast<double>(total_bytes) / 256.0;

    double chi_squared = 0.0;
    for (const auto& count : byte_counts) {
        double diff = static_cast<double>(count) - expected;
        chi_squared += (diff * diff) / expected;
    }

    // Critical value for chi-squared with 255 degrees of freedom at 0.001 significance
    // This gives us 99.9% confidence that random data will pass
    constexpr double CRITICAL_VALUE = 310.5;  // Chi-squared(255, 0.001)

    // Also check that no single byte value dominates (would indicate a pattern, not random)
    uint64_t max_count = *std::max_element(byte_counts.begin(), byte_counts.end());
    double max_ratio = static_cast<double>(max_count) / static_cast<double>(total_bytes);

    // If any single byte is more than 1% of total (vs expected ~0.39%), suspicious
    constexpr double MAX_BYTE_RATIO = 0.01;

    return chi_squared < CRITICAL_VALUE && max_ratio < MAX_BYTE_RATIO;
}

auto verify_buffer_pattern(int fd, uint64_t size,
                           const std::vector<uint8_t>& expected_pattern,
                           ProgressCallback callback,
                           const std::atomic<bool>& cancel_flag) -> bool {
    if (size == 0 || expected_pattern.empty())
        return true;

    // Seek to beginning
    if (lseek(fd, 0, SEEK_SET) != 0) {
        return false;
    }

    std::vector<uint8_t> buffer(VERIFY_BUFFER_SIZE);
    uint64_t verified = 0;
    bool mismatch_found = false;

    while (verified < size && !cancel_flag.load()) {
        size_t to_read = std::min(static_cast<uint64_t>(buffer.size()), size - verified);
        ssize_t bytes_read = read_with_retry(fd, buffer.data(), to_read);

        if (bytes_read <= 0) {
            return false;  // Read error
        }

        // Check bytes match expected pattern (repeating)
        for (ssize_t i = 0; i < bytes_read; ++i) {
            size_t pattern_idx = (verified + static_cast<uint64_t>(i)) % expected_pattern.size();
            if (buffer[static_cast<size_t>(i)] != expected_pattern[pattern_idx]) {
                mismatch_found = true;
                break;
            }
        }

        if (mismatch_found) {
            break;
        }

        verified += static_cast<uint64_t>(bytes_read);
        emit_progress(callback, verified, size);
    }

    return !mismatch_found && !cancel_flag.load();
}

}  // namespace verification
