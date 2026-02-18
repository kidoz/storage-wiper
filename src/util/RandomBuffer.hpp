#ifndef STORAGE_WIPER_UTIL_RANDOM_BUFFER_HPP
#define STORAGE_WIPER_UTIL_RANDOM_BUFFER_HPP

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace util {

/**
 * @class RandomBufferGenerator
 * @brief Helper class for efficient random buffer generation
 */
class RandomBufferGenerator {
public:
    /**
     * @brief Fills a buffer with random bytes using a 64-bit generator for efficiency
     * @param buffer The buffer to fill
     */
    static void fill(std::vector<uint8_t>& buffer) {
        // Use thread_local engine to avoid initialization overhead on every call
        thread_local std::mt19937_64 generator{std::random_device{}()};

        // Fill 64-bit chunks for speed
        size_t size = buffer.size();
        size_t u64_count = size / sizeof(uint64_t);

        for (size_t i = 0; i < u64_count; ++i) {
            uint64_t val = generator();
            std::memcpy(buffer.data() + i * sizeof(uint64_t), &val, sizeof(uint64_t));
        }

        // Fill remaining bytes
        size_t remaining = size % sizeof(uint64_t);
        if (remaining > 0) {
            uint64_t last_chunk = generator();
            std::memcpy(buffer.data() + u64_count * sizeof(uint64_t), &last_chunk, remaining);
        }
    }
};

} // namespace util

#endif // STORAGE_WIPER_UTIL_RANDOM_BUFFER_HPP
