#pragma once

#include <cerrno>
#include <unistd.h>

namespace util {

inline auto write_with_retry(int fd, const void* buffer, size_t size) -> ssize_t {
    while (true) {
        const auto result = ::write(fd, buffer, size);
        if (result < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        return result;
    }
}

} // namespace util
