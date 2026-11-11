#ifndef STORAGE_WIPER_TESTS_FAKE_DEVICE_IO_HPP
#define STORAGE_WIPER_TESTS_FAKE_DEVICE_IO_HPP

#include <sys/stat.h>
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
        bool fail_position_restore = false;
        int writes = 0;
        int claims = 0;
        int flushes = 0;
        uint64_t device_number = 259;
        uint64_t disk_sequence = 1;
        bool disk_sequence_supported = true;
        std::vector<off_t> retry_offsets;
    };

    FakeDeviceIO();
    ~FakeDeviceIO();
    FakeDeviceIO(const FakeDeviceIO&) = delete;
    auto operator=(const FakeDeviceIO&) -> FakeDeviceIO& = delete;

    std::map<std::string, Device> devices;
    bool format_only = false;
    int namespace_id = 7;
    uint32_t namespace_count = 1;
    uint8_t controller_multipath = 0;
    uint8_t namespace_sharing = 0;
    uint8_t format_attributes = 4;
    uint32_t formatted_namespace = 0;
    uint32_t format_command = 0;
    int sanitize_commands = 0;
    int sanitize_log_reads = 0;
    bool fail_first_sanitize_log_read = false;
    bool previous_sanitize_succeeded = false;
    bool all_namespaces_claimed = false;
    bool ata_password_enabled = false;
    bool fail_ata_disable_password = false;
    bool fail_ata_prepare = false;
    bool fail_ata_erase = false;
    int ata_disable_attempts = 0;
    std::function<void()> before_write;
    std::function<void()> before_identify;
    std::function<void()> during_firmware_command;
    std::function<void(uint8_t)> during_ata_command;
    int opens = 0;

    auto open(const char* path, int flags) -> int;
    auto close(int fd) -> int;
    auto read(int fd, void* buffer, size_t count) -> ssize_t;
    auto write(int fd, const void* buffer, size_t count) -> ssize_t;
    auto pwrite(int fd, const void* buffer, size_t count, off_t offset) -> ssize_t;
    auto seek(int fd, off_t offset, int whence) -> off_t;
    auto ioctl(int fd, unsigned long request, void* argument) -> int;
    auto flush(int fd) -> int;
    auto stat(int fd, struct stat64* status) -> int;

    static FakeDeviceIO* active;
    static constexpr int FIRST_FD = 90'000;

private:
    struct Descriptor {
        std::string path;
        off_t offset = 0;
        int flags = 0;
        uint64_t device_number = 0;
        uint64_t disk_sequence = 0;
    };
    std::mutex mutex_;
    std::map<int, Descriptor> descriptors_;
    int next_fd_ = FIRST_FD;
};

#endif
