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
#include <span>

namespace verification {

namespace {
constexpr size_t VERIFY_BUFFER_SIZE = 1'024 * 1'024;  // 1MB buffer

/**
 * @brief Read with retry on EINTR
 */
auto read_with_retry(int fd, std::span<uint8_t> buf) -> ssize_t {
    while (true) {
        ssize_t result = ::read(fd, buf.data(), buf.size());
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
    progress.verification_percentage =
        (static_cast<double>(verified) / static_cast<double>(total)) * 100.0;
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

auto verify_pattern(int fd, uint64_t size, uint8_t pattern, ProgressCallback callback,
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
        ssize_t bytes_read = read_with_retry(fd, std::span<uint8_t>(buffer.data(), to_read));

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

}  // namespace verification
