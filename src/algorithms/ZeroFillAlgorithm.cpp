#include "algorithms/ZeroFillAlgorithm.hpp"

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

    while (written < size && !cancel_flag.load()) {
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
            progress.current_pass = 1;
            progress.total_passes = 1;
            progress.percentage =
                (static_cast<double>(written) / static_cast<double>(size)) * 100.0;
            progress.status = "Writing zeros...";
            callback(progress);
        }
    }

    return !cancel_flag.load();
}
