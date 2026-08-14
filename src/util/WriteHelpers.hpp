#pragma once

#include <poll.h>
#include <unistd.h>

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

}  // namespace util
