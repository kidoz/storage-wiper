#include "util/SecureRandom.hpp"

#include "util/Logger.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

namespace util {

namespace {

[[noreturn]] void fatal(const char* what, int err) noexcept {
    LOG_ERROR("SecureRandom",
              std::format("{} failed: {} (errno={})", what, std::strerror(err), err));
    std::abort();
}

// Lazily-opened, process-wide /dev/urandom fallback. The fd lives until
// process exit; re-opening per miss would cost more than the fd itself.
auto urandom_fd() noexcept -> int {
    static const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    return fd;
}

void fill_from_urandom(unsigned char* dst, std::size_t remaining) noexcept {
    const int fd = urandom_fd();
    if (fd < 0) {
        fatal("/dev/urandom open", errno);
    }
    while (remaining > 0) {
        const ssize_t n = ::read(fd, dst, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fatal("/dev/urandom read", errno);
        }
        if (n == 0) {
            // /dev/urandom should never EOF; treat as fatal corruption.
            fatal("/dev/urandom unexpected EOF", 0);
        }
        dst += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

}  // namespace

void secure_random_fill(std::span<std::byte> out) noexcept {
    if (out.empty()) {
        return;
    }
    auto* dst = reinterpret_cast<unsigned char*>(out.data());
    std::size_t remaining = out.size();
    while (remaining > 0) {
        const ssize_t n = ::getrandom(dst, remaining, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOSYS) {
                // Pre-3.17 kernel: fall through to /dev/urandom for the rest.
                fill_from_urandom(dst, remaining);
                return;
            }
            fatal("getrandom", errno);
        }
        dst += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

}  // namespace util
