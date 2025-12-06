#include "algorithms/DoD522022MAlgorithm.hpp"
#include "interfaces/IWipeService.hpp"
#include <algorithm>
#include <format>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

bool DoD522022MAlgorithm::execute(int fd, uint64_t size, ProgressCallback callback,
                                  const std::atomic<bool>& cancel_flag) {
    // Handle zero-size case
    if (size == 0) {
        return true;
    }

    // Pass 1: Zero fill
    std::vector<uint8_t> zeros(BUFFER_SIZE, 0x00);
    if (!write_pattern(fd, size, zeros.data(), zeros.size(), callback, 1, 3, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1) return false;

    // Pass 2: Ones fill
    std::vector<uint8_t> ones(BUFFER_SIZE, 0xFF);
    if (!write_pattern(fd, size, ones.data(), ones.size(), callback, 2, 3, cancel_flag)) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == -1) return false;

    // Pass 3: Random data
    std::random_device random_device;
    std::mt19937 random_generator(random_device());
    std::uniform_int_distribution<uint8_t> byte_distribution(0, 255);

    std::vector<uint8_t> random_buffer(BUFFER_SIZE);
    uint64_t written = 0;

    while (written < size && !cancel_flag.load()) {
        // Generate fresh random data for each buffer
        for (auto& byte : random_buffer) {
            byte = byte_distribution(random_generator);
        }

        size_t to_write = std::min(static_cast<uint64_t>(BUFFER_SIZE), size - written);
        ssize_t result = write(fd, random_buffer.data(), to_write);

        if (result <= 0) {
            return false;
        }

        written += static_cast<uint64_t>(result);

        if (callback) {
            WipeProgress progress{};
            progress.bytes_written = written;
            progress.total_bytes = size;
            progress.current_pass = 3;
            progress.total_passes = 3;
            progress.percentage = (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing pattern (Pass 3/3)";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}

bool DoD522022MAlgorithm::write_pattern(int fd, uint64_t size, const uint8_t* pattern,
                                       size_t pattern_size, ProgressCallback callback,
                                       int pass, int total_passes,
                                       const std::atomic<bool>& cancel_flag) {
    uint64_t written = 0;

    while (written < size && !cancel_flag.load()) {
        size_t to_write = std::min(pattern_size, size - written);
        ssize_t result = write(fd, pattern, to_write);

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
            progress.status = std::format("Writing pattern (Pass {}/{})", pass, total_passes);
            callback(progress);
        }
    }

    return !cancel_flag.load();
}
