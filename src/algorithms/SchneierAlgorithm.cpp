#include "algorithms/SchneierAlgorithm.hpp"

#include "models/WipeTypes.hpp"
#include "util/RandomBuffer.hpp"
#include "util/WriteHelpers.hpp"

#include <unistd.h>

#include <algorithm>
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
    if (!write_pattern(fd, size, buffer, callback, 1, 7, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1)
        return false;

    // Pass 2: 0x00
    std::fill(buffer.begin(), buffer.end(), 0x00);
    if (!write_pattern(fd, size, buffer, callback, 2, 7, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1)
        return false;

    // Passes 3-7: Random data
    for (int pass = 3; pass <= 7; ++pass) {
        uint64_t written = 0;
        uint64_t bad_blocks = 0;

        while (written < size && !cancel_flag.load()) {
            // Generate fresh random data for each buffer
            util::RandomBufferGenerator::fill(buffer);

            size_t to_write = std::min(static_cast<uint64_t>(BUFFER_SIZE), size - written);
            uint64_t bad = 0;
            const auto result = util::write_with_bad_sector_tolerance(
                fd, std::span<const uint8_t>(buffer.data(), to_write), bad);

            if (result == 0) {
                return false;
            }
            bad_blocks += bad;

            written += result;

            if (callback) {
                WipeProgress progress{};
                progress.bytes_written = written;
                progress.total_bytes = size;
                progress.current_pass = pass;
                progress.total_passes = 7;
                progress.percentage =
                    (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
                progress.status = "Writing pattern (Pass " + std::to_string(pass) + "/7)";
                progress.bad_block_count = bad_blocks;
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

bool SchneierAlgorithm::write_pattern(int fd, uint64_t size, std::span<const uint8_t> pattern,
                                      ProgressCallback callback, int pass, int total_passes,
                                      const std::atomic<bool>& cancel_flag) {
    uint64_t written = 0;
    uint64_t bad_blocks = 0;

    while (written < size && !cancel_flag.load()) {
        size_t to_write = std::min(pattern.size(), size - written);
        uint64_t bad = 0;
        const auto result = util::write_with_bad_sector_tolerance(
            fd, std::span<const uint8_t>(pattern.data(), to_write), bad);

        if (result == 0) {
            return false;
        }
        bad_blocks += bad;

        written += result;

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
            progress.bad_block_count = bad_blocks;
            callback(progress);
        }
    }

    return !cancel_flag.load();
}
