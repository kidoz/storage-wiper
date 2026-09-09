#include "fixtures/FakeDeviceIO.hpp"

#include "algorithms/NvmeSanitize.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>

FakeDeviceIO* FakeDeviceIO::active = nullptr;

FakeDeviceIO::FakeDeviceIO() {
    active = this;
}
FakeDeviceIO::~FakeDeviceIO() {
    active = nullptr;
}

auto FakeDeviceIO::open(const char* path, int flags) -> int {
    std::lock_guard lock(mutex_);
    auto& device = devices.at(path);
    if ((flags & O_EXCL) != 0) {
        if (device.busy || device.claims != 0) {
            errno = EBUSY;
            return -1;
        }
        ++device.claims;
    }
    const int fd = next_fd_++;
    descriptors_[fd] = {.path = path, .flags = flags};
    return fd;
}

auto FakeDeviceIO::close(int fd) -> int {
    std::lock_guard lock(mutex_);
    auto& descriptor = descriptors_.at(fd);
    if ((descriptor.flags & O_EXCL) != 0) {
        --devices.at(descriptor.path).claims;
    }
    descriptors_.erase(fd);
    return 0;
}

auto FakeDeviceIO::read(int fd, void* buffer, size_t count) -> ssize_t {
    std::lock_guard lock(mutex_);
    auto& descriptor = descriptors_.at(fd);
    const auto& bytes = devices.at(descriptor.path).bytes;
    const size_t offset = static_cast<size_t>(descriptor.offset);
    count = std::min(count, bytes.size() - std::min(offset, bytes.size()));
    std::memcpy(buffer, bytes.data() + offset, count);
    descriptor.offset += static_cast<off_t>(count);
    return static_cast<ssize_t>(count);
}

auto FakeDeviceIO::write(int fd, const void* buffer, size_t count) -> ssize_t {
    if (before_write) {
        before_write();
    }
    std::lock_guard lock(mutex_);
    auto& descriptor = descriptors_.at(fd);
    auto& device = devices.at(descriptor.path);
    ++device.writes;
    if (device.fail_writes || (device.short_then_error && device.writes == 2)) {
        errno = EIO;
        return -1;
    }
    if (device.short_then_error && device.writes == 1) {
        count = std::min(count, size_t{512});
    }
    const size_t offset = static_cast<size_t>(descriptor.offset);
    device.bytes.resize(std::max(device.bytes.size(), offset + count));
    std::memcpy(device.bytes.data() + offset, buffer, count);
    descriptor.offset += static_cast<off_t>(count);
    return static_cast<ssize_t>(count);
}

auto FakeDeviceIO::pwrite(int fd, const void* buffer, size_t count, off_t offset) -> ssize_t {
    std::lock_guard lock(mutex_);
    auto& device = devices.at(descriptors_.at(fd).path);
    device.retry_offsets.push_back(offset);
    if (device.fail_writes) {
        errno = EIO;
        return -1;
    }
    device.bytes.resize(std::max(device.bytes.size(), static_cast<size_t>(offset) + count));
    std::memcpy(device.bytes.data() + offset, buffer, count);
    return static_cast<ssize_t>(count);
}

auto FakeDeviceIO::seek(int fd, off_t offset, int whence) -> off_t {
    std::lock_guard lock(mutex_);
    auto& descriptor = descriptors_.at(fd);
    descriptor.offset = whence == SEEK_SET ? offset : descriptor.offset + offset;
    return descriptor.offset;
}

auto FakeDeviceIO::flush(int fd) -> int {
    std::lock_guard lock(mutex_);
    ++devices.at(descriptors_.at(fd).path).flushes;
    return 0;
}

auto FakeDeviceIO::ioctl(int fd, unsigned long request, void* argument) -> int {
    if (request == NVME_IOCTL_ID) {
        return namespace_id;
    }
    if (request == NVME_IOCTL_ADMIN_CMD && static_cast<nvme_admin_cmd*>(argument)->opcode == 0x06 &&
        before_identify) {
        before_identify();
    }
    std::lock_guard lock(mutex_);
    if (request == BLKGETSIZE64) {
        *static_cast<uint64_t*>(argument) = devices.at(descriptors_.at(fd).path).bytes.size();
        return 0;
    }
    if (request != NVME_IOCTL_ADMIN_CMD) {
        errno = ENOTTY;
        return -1;
    }
    auto& cmd = *static_cast<nvme_admin_cmd*>(argument);
    auto* data = reinterpret_cast<uint8_t*>(cmd.addr);
    if (cmd.opcode == 0x06) {
        std::memset(data, 0, cmd.data_len);
        if (cmd.cdw10 == 1) {
            data[nvme_sanitize::ID_CTRL_OFFSET_OACS] = 2;
            data[nvme_sanitize::ID_CTRL_OFFSET_FNA] = 4;
            data[nvme_sanitize::ID_CTRL_OFFSET_SANICAP] = format_only ? 0 : 1;
        } else {
            data[nvme_sanitize::ID_NS_OFFSET_FLBAS] = 3;
        }
    } else if (cmd.opcode == 0x02) {
        std::memset(data, 0, cmd.data_len);
        data[2] = sanitize_commands > 0 ? 1 : 0;
    } else if (cmd.opcode == 0x84 || cmd.opcode == 0x80) {
        all_namespaces_claimed = true;
        for (const auto& [path, device] : devices) {
            if (nvme_sanitize::controller_path_of(path) && device.claims != 1) {
                all_namespaces_claimed = false;
            }
        }
        if (cmd.opcode == 0x84) {
            ++sanitize_commands;
        } else {
            formatted_namespace = cmd.nsid;
            format_command = cmd.cdw10;
        }
    }
    return 0;
}

// GNU ld requires these exact reserved symbol names for --wrap.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" {
int __real_open64(const char*, int, ...);
int __real_close(int);
ssize_t __real_write(int, const void*, size_t);
ssize_t __real_pwrite64(int, const void*, size_t, off_t);
ssize_t __real_read(int, void*, size_t);
off_t __real_lseek64(int, off_t, int);
int __real_fsync(int);
int __real_ioctl(int, unsigned long, ...);

int __wrap_open64(const char* path, int flags, ...) {
    if (FakeDeviceIO::active && FakeDeviceIO::active->devices.contains(path)) {
        return FakeDeviceIO::active->open(path, flags);
    }
    // Tests must never accidentally open an unregistered real disk.
    if (std::string_view{path}.starts_with("/dev/") && std::string_view{path} != "/dev/urandom") {
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        const auto mode = va_arg(args, mode_t);
        va_end(args);
        return __real_open64(path, flags, mode);
    }
    return __real_open64(path, flags);
}

int __wrap_close(int fd) {
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->close(fd) : __real_close(fd);
}
ssize_t __wrap_read(int fd, void* buffer, size_t count) {
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->read(fd, buffer, count)
                                        : __real_read(fd, buffer, count);
}
ssize_t __wrap_write(int fd, const void* buffer, size_t count) {
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->write(fd, buffer, count)
                                        : __real_write(fd, buffer, count);
}
ssize_t __wrap_pwrite64(int fd, const void* buffer, size_t count, off_t offset) {
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->pwrite(fd, buffer, count, offset)
                                        : __real_pwrite64(fd, buffer, count, offset);
}
off_t __wrap_lseek64(int fd, off_t offset, int whence) {
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->seek(fd, offset, whence)
                                        : __real_lseek64(fd, offset, whence);
}
int __wrap_fsync(int fd) {
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->flush(fd) : __real_fsync(fd);
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    void* argument = nullptr;
    if (request != NVME_IOCTL_ID) {
        va_list args;
        va_start(args, request);
        argument = va_arg(args, void*);
        va_end(args);
    }
    return fd >= FakeDeviceIO::FIRST_FD ? FakeDeviceIO::active->ioctl(fd, request, argument)
                                        : __real_ioctl(fd, request, argument);
}
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
