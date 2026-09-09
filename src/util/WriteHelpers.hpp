#pragma once

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <span>

namespace util {

inline auto write_with_retry(int fd, std::span<const uint8_t> data) -> ssize_t {
    while (true) {
        const auto result = ::write(fd, data.data(), data.size());
        if (result >= 0) {
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            // Non-blocking fd: wait for writability instead of busy-spinning.
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            ::poll(&pfd, 1, -1);
            continue;
        }
        return result;
    }
}

namespace detail {

// Sector granularity at which bad sectors are skipped
constexpr size_t BAD_BLOCK_SECTOR = 512;
// Retries per sector before it is declared bad
constexpr int BAD_BLOCK_ATTEMPTS = 3;

/**
 * @brief Sector-by-sector fallback for a region the device refused to write
 *
 * Writes 512-byte chunks at explicit absolute offsets, counting the sectors
 * that still fail after BAD_BLOCK_ATTEMPTS attempts instead of aborting. The
 * caller must reposition the file descriptor past the handled region
 * afterwards, because these explicit-offset writes do not advance it.
 *
 * @param pwrite_fn Callable (offset, chunk) -> bytes written, or -1 on error
 * @param base Absolute device offset that corresponds to data[0]
 * @param done Bytes already consumed from data by earlier writes
 * @param data The full buffer handed to the tolerant write
 * @param skipped Incremented by the number of skipped sectors
 * @return Total bytes consumed from data (written plus skipped)
 */
template <typename PWriteFn>
auto sector_skipping_write(PWriteFn pwrite_fn, off_t base, size_t done,
                           std::span<const uint8_t> data, uint64_t& skipped) -> size_t {
    off_t absolute = base + static_cast<off_t>(done);
    while (done < data.size()) {
        const size_t chunk = std::min<size_t>(BAD_BLOCK_SECTOR, data.size() - done);
        bool written = false;
        for (int attempt = 0; attempt < BAD_BLOCK_ATTEMPTS && !written; ++attempt) {
            const auto result =
                pwrite_fn(absolute, std::span<const uint8_t>(data.data() + done, chunk));
            if (result == static_cast<ssize_t>(chunk)) {
                written = true;
            } else if (result < 0) {
                break;  // persistent device error: this sector is bad
            }
        }
        if (!written) {
            ++skipped;
        }
        done += chunk;
        absolute += static_cast<off_t>(chunk);
    }
    return done;
}

}  // namespace detail

/**
 * @brief Write a buffer, tolerating unwritable (bad) sectors
 *
 * Tries plain sequential writes first. If the device reports a write error
 * (typically a bad sector on a failing drive), the remaining region is
 * retried sector by sector at explicit offsets; sectors that keep failing
 * are skipped and counted, and the wipe continues instead of aborting. The
 * file descriptor position always ends up past the handled region, so
 * subsequent sequential writes continue at the right spot.
 *
 * @param fd File descriptor to write to
 * @param data Buffer to write
 * @param bad_blocks Set to the number of skipped 512-byte sectors
 * @return Bytes consumed from the buffer (written plus skipped); 0 means the
 *         write failed before anything could be delivered and the caller
 *         should abort
 */
inline auto write_with_bad_sector_tolerance(int fd, std::span<const uint8_t> data,
                                            uint64_t& bad_blocks) -> uint64_t {
    bad_blocks = 0;

    // Fast path: plain sequential writes while the device cooperates
    size_t done = 0;
    while (done < data.size()) {
        const auto result = write_with_retry(fd, data.subspan(done));
        if (result > 0) {
            done += static_cast<size_t>(result);
            continue;
        }
        break;  // write error: switch to sector-skipping
    }
    if (done == data.size()) {
        return done;
    }

    // Locate the current device offset so the fallback can address the
    // remaining region explicitly.
    const off_t base = ::lseek(fd, 0, SEEK_CUR);
    if (base < 0) {
        return done;
    }

    uint64_t skipped = 0;
    done = detail::sector_skipping_write(
        [fd](off_t offset, std::span<const uint8_t> chunk) -> ssize_t {
            for (int attempt = 0; attempt < detail::BAD_BLOCK_ATTEMPTS; ++attempt) {
                const auto result = ::pwrite(fd, chunk.data(), chunk.size(), offset);
                if (result == static_cast<ssize_t>(chunk.size())) {
                    return result;
                }
                if (result < 0 && errno != EINTR) {
                    return -1;
                }
            }
            return -1;
        },
        base, done, data, skipped);

    // Keep the sequential position in sync: skipped sectors still consume
    // device space, so the next plain write must land after them.
    ::lseek(fd, base + static_cast<off_t>(done), SEEK_SET);

    bad_blocks = skipped;
    return done;
}

}  // namespace util
