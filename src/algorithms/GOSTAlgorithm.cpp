#include "algorithms/GOSTAlgorithm.hpp"

#include "models/WipeTypes.hpp"
#include "util/RandomBuffer.hpp"
#include "util/WriteHelpers.hpp"

#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

bool GOSTAlgorithm::execute(int fd, uint64_t size, ProgressCallback callback,
                            const std::atomic<bool>& cancel_flag) {
    // Handle zero-size case
    if (size == 0) {
        return true;
    }

    // GOST R 50739-95: 2-pass method
    // Pass 1: Zeros (0x00)
    // Pass 2: Random data

    // Pass 1: Zero fill
    std::vector<uint8_t> zeros(BUFFER_SIZE, 0x00);
    if (!write_pattern(fd, size, zeros, callback, 1, 2, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1)
        return false;

    // Pass 2: Random data
    std::vector<uint8_t> random_buffer(BUFFER_SIZE);
    uint64_t written = 0;

    while (written < size && !cancel_flag.load()) {
        // Generate fresh random data for each buffer
        util::RandomBufferGenerator::fill(random_buffer);

        size_t to_write = std::min(static_cast<uint64_t>(BUFFER_SIZE), size - written);
        ssize_t result =
            util::write_with_retry(fd, std::span<const uint8_t>(random_buffer.data(), to_write));

        if (result <= 0) {
            return false;
        }

        written += static_cast<uint64_t>(result);

        if (callback) {
            WipeProgress progress{};
            progress.bytes_written = written;
            progress.total_bytes = size;
            progress.current_pass = 2;
            progress.total_passes = 2;
            progress.percentage =
                (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing pattern (Pass 2/2)";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}

bool GOSTAlgorithm::write_pattern(int fd, uint64_t size, std::span<const uint8_t> pattern,
                                  ProgressCallback callback, int pass, int total_passes,
                                  const std::atomic<bool>& cancel_flag) {
    uint64_t written = 0;

    while (written < size && !cancel_flag.load()) {
        size_t to_write = std::min(pattern.size(), size - written);
        ssize_t result =
            util::write_with_retry(fd, std::span<const uint8_t>(pattern.data(), to_write));

        if (result <= 0) {
            return false;
        }

        written += static_cast<uint64_t>(result);

        if (callback) {
            WipeProgress progress{};
            progress.bytes_written = written;
            progress.total_bytes = size;
            progress.current_pass = pass;
            progress.total_passes = total_passes;
            progress.percentage =
                (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing pattern (Pass " + std::to_string(pass) + "/" +
                              std::to_string(total_passes) + ")";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}
