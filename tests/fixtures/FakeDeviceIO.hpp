#ifndef STORAGE_WIPER_TESTS_FAKE_DEVICE_IO_HPP
#define STORAGE_WIPER_TESTS_FAKE_DEVICE_IO_HPP

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Used only by the separate syscall-wrapped test executable. Fake descriptors
// never reach the kernel; all unregistered paths use the real system calls.
class FakeDeviceIO {
public:
    struct Device {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(2'048, 0);
        bool busy = false;
        bool fail_writes = false;
        bool short_then_error = false;
        int writes = 0;
        int claims = 0;
        int flushes = 0;
        std::vector<off_t> retry_offsets;
    };

    FakeDeviceIO();
    ~FakeDeviceIO();
    FakeDeviceIO(const FakeDeviceIO&) = delete;
    auto operator=(const FakeDeviceIO&) -> FakeDeviceIO& = delete;

    std::map<std::string, Device> devices;
    bool format_only = false;
    int namespace_id = 7;
    uint32_t formatted_namespace = 0;
    uint32_t format_command = 0;
    int sanitize_commands = 0;
    bool all_namespaces_claimed = false;
    std::function<void()> before_write;
    std::function<void()> before_identify;

    auto open(const char* path, int flags) -> int;
    auto close(int fd) -> int;
    auto read(int fd, void* buffer, size_t count) -> ssize_t;
    auto write(int fd, const void* buffer, size_t count) -> ssize_t;
    auto pwrite(int fd, const void* buffer, size_t count, off_t offset) -> ssize_t;
    auto seek(int fd, off_t offset, int whence) -> off_t;
    auto ioctl(int fd, unsigned long request, void* argument) -> int;
    auto flush(int fd) -> int;

    static FakeDeviceIO* active;
    static constexpr int FIRST_FD = 90'000;

private:
    struct Descriptor {
        std::string path;
        off_t offset = 0;
        int flags = 0;
    };
    std::mutex mutex_;
    std::map<int, Descriptor> descriptors_;
    int next_fd_ = FIRST_FD;
};

#endif
