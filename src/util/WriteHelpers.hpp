#pragma once

#include <unistd.h>
#include <cerrno>
#include <span>
#include <cstdint>

namespace util {

inline auto write_with_retry(int fd, std::span<const uint8_t> data) -> ssize_t {
    while (true) {
        const auto result = ::write(fd, data.data(), data.size());
        if (result < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        return result;
    }
}

}  // namespace util
