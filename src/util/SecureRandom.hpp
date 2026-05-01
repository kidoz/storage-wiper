#ifndef STORAGE_WIPER_UTIL_SECURE_RANDOM_HPP
#define STORAGE_WIPER_UTIL_SECURE_RANDOM_HPP

#include <cstddef>
#include <span>

namespace util {

// Fill `out` with cryptographically secure random bytes from the OS CSPRNG
// via getrandom(2), with a /dev/urandom fallback for kernels < 3.17.
// Loops on partial reads and EINTR. Empty spans are a no-op.
//
// On unrecoverable kernel failure (which should not happen on a working
// Linux system), logs an error and aborts. Silently degrading to a
// non-cryptographic generator would defeat the purpose of a security tool
// that markets DoD/Schneier/Gutmann passes; loud failure is the only
// acceptable mode here.
void secure_random_fill(std::span<std::byte> out) noexcept;

}  // namespace util

#endif  // STORAGE_WIPER_UTIL_SECURE_RANDOM_HPP
