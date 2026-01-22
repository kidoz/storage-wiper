#include "algorithms/SchneierAlgorithm.hpp"

#include "models/WipeTypes.hpp"
#include "util/WriteHelpers.hpp"

#include <unistd.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

bool SchneierAlgorithm::execute(int fd, uint64_t size, ProgressCallback callback,
                                const std::atomic<bool>& cancel_flag) {
    // Handle zero-size case
    if (size == 0) {
        return true;
    }

    // Schneier 7-pass: 0xFF, 0x00, then 5 random passes
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    // Pass 1: 0xFF
    std::fill(buffer.begin(), buffer.end(), 0xFF);
    if (!write_pattern(fd, size, buffer.data(), buffer.size(), callback, 1, 7, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1)
        return false;

    // Pass 2: 0x00
    std::fill(buffer.begin(), buffer.end(), 0x00);
    if (!write_pattern(fd, size, buffer.data(), buffer.size(), callback, 2, 7, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1)
        return false;

    // Passes 3-7: Random data
    std::random_device random_device;
    std::mt19937 random_generator(random_device());
    std::uniform_int_distribution<uint8_t> byte_distribution(0, 255);

    for (int pass = 3; pass <= 7; ++pass) {
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
                progress.current_pass = pass;
                progress.total_passes = 7;
                progress.percentage =
                    (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
                progress.status = "Writing pattern (Pass " + std::to_string(pass) + "/7)";
                callback(progress);
            }
        }

        if (cancel_flag.load()) {
            return false;
        }

        if (pass < 7) {
            if (lseek(fd, 0, SEEK_SET) == -1)
                return false;
        }
    }

    return true;
}

bool SchneierAlgorithm::write_pattern(int fd, uint64_t size, const uint8_t* pattern,
                                      size_t pattern_size, ProgressCallback callback, int pass,
                                      int total_passes, const std::atomic<bool>& cancel_flag) {
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
            progress.percentage =
                (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing pattern (Pass " + std::to_string(pass) + "/" +
                              std::to_string(total_passes) + ")";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}
