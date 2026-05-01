#ifndef STORAGE_WIPER_UTIL_RANDOM_BUFFER_HPP
#define STORAGE_WIPER_UTIL_RANDOM_BUFFER_HPP

#include "util/SecureRandom.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace util {

/**
 * @class RandomBufferGenerator
 * @brief Fills wipe buffers with cryptographically secure random bytes.
 *
 * Backed by util::secure_random_fill, which prefers getrandom(2) and falls
 * back to /dev/urandom only when the syscall is unavailable (kernel < 3.17).
 * getrandom blocks until the kernel pool is seeded at early boot, which
 * /dev/urandom does not - meaningful when wiping early in a recovery image.
 */
class RandomBufferGenerator {
public:
    static void fill(std::vector<uint8_t>& buffer) noexcept {
        if (buffer.empty()) {
            return;
        }
        secure_random_fill(std::as_writable_bytes(std::span{buffer}));
    }
};

}  // namespace util

#endif  // STORAGE_WIPER_UTIL_RANDOM_BUFFER_HPP
