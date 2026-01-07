#include "algorithms/VSITRAlgorithm.hpp"
#include "models/WipeTypes.hpp"
#include "util/WriteHelpers.hpp"
#include <algorithm>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

bool VSITRAlgorithm::execute(int fd, uint64_t size, ProgressCallback callback,
                             const std::atomic<bool>& cancel_flag) {
    // Handle zero-size case
    if (size == 0) {
        return true;
    }

    // VSITR 7-pass: alternating 0x00, 0xFF patterns with random passes
    const uint8_t patterns[] = {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF};
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    // Passes 1-6: Alternating patterns
    for (int pass = 1; pass <= 6; ++pass) {
        std::fill(buffer.begin(), buffer.end(), patterns[pass - 1]);
        if (!write_pattern(fd, size, buffer.data(), buffer.size(), callback, pass, 7, cancel_flag)) {
            return false;
        }
        if (lseek(fd, 0, SEEK_SET) == -1) return false;
    }

    // Pass 7: Random data
    std::random_device random_device;
    std::mt19937 random_generator(random_device());
    std::uniform_int_distribution<uint8_t> byte_distribution(0, 255);

    uint64_t written = 0;

    while (written < size && !cancel_flag.load()) {
        // Generate fresh random data for each buffer
        for (auto& byte : buffer) {
            byte = byte_distribution(random_generator);
        }

        size_t to_write = std::min(static_cast<uint64_t>(BUFFER_SIZE), size - written);
        ssize_t result = util::write_with_retry(fd, buffer.data(), to_write);

        if (result <= 0) {
            return false;
        }

        written += static_cast<uint64_t>(result);

        if (callback) {
            WipeProgress progress{};
            progress.bytes_written = written;
            progress.total_bytes = size;
            progress.current_pass = 7;
            progress.total_passes = 7;
            progress.percentage = (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing pattern (Pass 7/7)";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}

bool VSITRAlgorithm::write_pattern(int fd, uint64_t size, const uint8_t* pattern,
                                  size_t pattern_size, ProgressCallback callback,
                                  int pass, int total_passes,
                                  const std::atomic<bool>& cancel_flag) {
    uint64_t written = 0;

    while (written < size && !cancel_flag.load()) {
        size_t to_write = std::min(pattern_size, size - written);
        ssize_t result = util::write_with_retry(fd, pattern, to_write);

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
            progress.percentage = (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing pattern (Pass " + std::to_string(pass) +
                            "/" + std::to_string(total_passes) + ")";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}
