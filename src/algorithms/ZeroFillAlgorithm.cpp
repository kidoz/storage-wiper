#include "algorithms/ZeroFillAlgorithm.hpp"

#include "algorithms/VerificationHelper.hpp"
#include "models/WipeTypes.hpp"
#include "util/WriteHelpers.hpp"

#include <unistd.h>

#include <algorithm>
#include <vector>

bool ZeroFillAlgorithm::execute(int fd, uint64_t size, ProgressCallback callback,
                                const std::atomic<bool>& cancel_flag) {
    // Handle zero-size case
    if (size == 0) {
        return true;
    }

    std::vector<uint8_t> buffer(BUFFER_SIZE, 0);
    uint64_t written = 0;
    uint64_t bad_blocks = 0;

    while (written < size && !cancel_flag.load()) {
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
            progress.current_pass = 1;
            progress.total_passes = 1;
            progress.percentage =
                (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing zeros...";
            progress.bad_block_count = bad_blocks;
            callback(progress);
        }
    }

    return !cancel_flag.load();
}

bool ZeroFillAlgorithm::verify(int fd, uint64_t size, ProgressCallback callback,
                               const std::atomic<bool>& cancel_flag) {
    return verification::verify_zeros(fd, size, std::move(callback), cancel_flag);
}
