/**
 * @file FileDescriptor.hpp
 * @brief RAII wrapper for POSIX file descriptors
 *
 * This file provides a RAII wrapper for managing file descriptor lifecycle,
 * ensuring automatic cleanup and preventing resource leaks.
 */

#pragma once

#include <unistd.h>

#include <utility>

namespace util {

/**
 * @class FileDescriptor
 * @brief RAII wrapper for POSIX file descriptors
 *
 * Provides automatic resource management for file descriptors with
 * move semantics and proper cleanup in destructor.
 */
class FileDescriptor {
public:
    /**
     * @brief Construct from raw file descriptor
     * @param fd Raw file descriptor (may be invalid)
     */
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    /**
     * @brief Destructor - closes file descriptor if valid
     */
    ~FileDescriptor() {
        if (is_valid()) {
            ::close(fd_);
        }
    }

    // Non-copyable
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    // Moveable
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (is_valid()) {
                ::close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    /**
     * @brief Get the raw file descriptor
     * @return Raw file descriptor value
     */
    [[nodiscard]] constexpr auto get() const noexcept -> int { return fd_; }

    /**
     * @brief Check if file descriptor is valid
     * @return true if fd >= 0, false otherwise
     */
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return fd_ >= 0; }

    /**
     * @brief Boolean conversion operator
     * @return true if file descriptor is valid
     */
    explicit operator bool() const noexcept { return is_valid(); }

private:
    int fd_;  ///< Raw file descriptor
};

}  // namespace util
