#include "algorithms/GutmannAlgorithm.hpp"

#include "models/WipeTypes.hpp"
#include "util/WriteHelpers.hpp"

#include <unistd.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

bool GutmannAlgorithm::execute(int fd, uint64_t size, ProgressCallback callback,
                               const std::atomic<bool>& cancel_flag) {
    // Handle zero-size case
    if (size == 0) {
        return true;
    }

    // Gutmann 35-pass algorithm (Simplified Modern Implementation)
    //
    // NOTE: This is a simplified implementation suitable for modern drives.
    // The original Gutmann method (1996) specified 22 specific patterns designed
    // for MFM/RLL encoded drives from the 1980s-1990s. Modern drives no longer
    // use these encoding schemes, making the specific patterns unnecessary.
    //
    // Peter Gutmann himself has stated that the full 35-pass with MFM/RLL patterns
    // is overkill for modern drives and that even a single pass is often sufficient
    // for current hardware due to increased track density.
    //
    // This implementation maintains the 35-pass structure (4 random + 27 patterns + 4 random)
    // but uses simplified patterns appropriate for modern drive technology.
    //
    // Reference: "Secure Deletion of Data from Magnetic and Solid-State Memory"
    // by Peter Gutmann, 1996

    std::vector<uint8_t> buffer(BUFFER_SIZE);
    std::random_device random_device;
    std::mt19937 random_generator(random_device());
    std::uniform_int_distribution<uint8_t> byte_distribution(0, 255);

    // Passes 1-4: Random data
    for (int pass = 1; pass <= 4; ++pass) {
        uint64_t written = 0;

        while (written < size && !cancel_flag.load()) {
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
                progress.total_passes = 35;
                progress.percentage =
                    (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
                progress.status = "Writing pattern (Pass " + std::to_string(pass) + "/35)";
                callback(progress);
            }
        }

        if (cancel_flag.load()) {
            return false;
        }

        if (lseek(fd, 0, SEEK_SET) == -1)
            return false;
    }

    // Passes 5-31: Specific patterns (simplified to alternating patterns)
    const uint8_t patterns[] = {0x55, 0xAA, 0x92, 0x49, 0x24, 0x00, 0x11, 0x22, 0x33,
                                0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
                                0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    for (int pass = 5; pass <= 31; ++pass) {
        std::fill(buffer.begin(), buffer.end(), patterns[(pass - 5) % 27]);
        if (!write_pattern(fd, size, buffer.data(), buffer.size(), callback, pass, 35,
                           cancel_flag)) {
            return false;
        }
        if (lseek(fd, 0, SEEK_SET) == -1)
            return false;
    }

    // Passes 32-35: Random data
    for (int pass = 32; pass <= 35; ++pass) {
        uint64_t written = 0;

        while (written < size && !cancel_flag.load()) {
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
                progress.total_passes = 35;
                progress.percentage =
                    (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
                progress.status = "Writing pattern (Pass " + std::to_string(pass) + "/35)";
                callback(progress);
            }
        }

        if (cancel_flag.load()) {
            return false;
        }

        if (pass < 35) {
            if (lseek(fd, 0, SEEK_SET) == -1)
                return false;
        }
    }

    return true;
}

bool GutmannAlgorithm::write_pattern(int fd, uint64_t size, const uint8_t* pattern,
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
