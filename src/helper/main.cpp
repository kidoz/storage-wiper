/**
 * @file helper/main.cpp
 * @brief Storage Wiper Helper - Privileged disk wiping utility
 *
 * This is a minimal privileged helper that performs the actual disk wiping.
 * It runs as root via pkexec and communicates with the GUI via stdout.
 *
 * Usage: storage-wiper-helper --device /dev/sdX --algorithm ALGO
 *
 * Output format (parsed by GUI):
 *   PROGRESS:<current_pass>:<total_passes>:<percentage>
 *   ERROR:<message>
 *   COMPLETE:success
 *   COMPLETE:cancelled
 *   COMPLETE:error:<message>
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <mntent.h>
#include <random>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>
#include <vector>

namespace {

// Global cancellation flag for signal handling
std::atomic<bool> g_cancelled{false};

// Buffer size for writing (4 MB for good performance)
constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;

// Supported algorithms
enum class Algorithm {
    ZERO_FILL,
    RANDOM_FILL,
    DOD_5220_22_M,
    SCHNEIER,
    VSITR,
    GOST_R_50739_95,
    GUTMANN
};

struct AlgorithmInfo {
    Algorithm algo;
    std::string_view name;
    int passes;
};

constexpr AlgorithmInfo ALGORITHMS[] = {
    {Algorithm::ZERO_FILL, "zero", 1},
    {Algorithm::RANDOM_FILL, "random", 1},
    {Algorithm::DOD_5220_22_M, "dod-5220-22-m", 3},
    {Algorithm::SCHNEIER, "schneier", 7},
    {Algorithm::VSITR, "vsitr", 7},
    {Algorithm::GOST_R_50739_95, "gost", 2},
    {Algorithm::GUTMANN, "gutmann", 35},
};

void signal_handler(int) {
    g_cancelled = true;
}

void report_progress(int current_pass, int total_passes, double percentage) {
    std::cout << std::format("PROGRESS:{}:{}:{:.1f}\n",
                             current_pass, total_passes, percentage);
    std::cout.flush();
}

void report_error(const std::string& message) {
    std::cerr << std::format("ERROR:{}\n", message);
    std::cerr.flush();
}

void report_complete(const std::string& status, const std::string& message = "") {
    if (message.empty()) {
        std::cout << std::format("COMPLETE:{}\n", status);
    } else {
        std::cout << std::format("COMPLETE:{}:{}\n", status, message);
    }
    std::cout.flush();
}

auto get_device_size(int fd) -> uint64_t {
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) != 0) {
        return 0;
    }
    return size;
}

auto validate_device_path(std::string_view path) -> bool {
    // Whitelist of allowed device prefixes
    constexpr std::string_view allowed_prefixes[] = {
        "/dev/sd",
        "/dev/nvme",
        "/dev/mmcblk",
        "/dev/vd"
    };

    for (const auto& prefix : allowed_prefixes) {
        if (path.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

namespace fs = std::filesystem;

/**
 * @brief Unmount all partitions of a device
 * @param device_path Path to the device (e.g., /dev/sda)
 * @return true if all partitions were unmounted successfully
 */
auto unmount_device(const std::string& device_path) -> bool {
    const auto device_name = fs::path{device_path}.filename().string();
    std::vector<std::string> mount_points;

    // Find all mounted partitions of this disk from /proc/mounts
    FILE* mtab = setmntent("/proc/mounts", "r");
    if (!mtab) {
        report_error("Failed to open /proc/mounts");
        return false;
    }

    while (auto* entry = getmntent(mtab)) {
        const std::string_view mount_device{entry->mnt_fsname};

        // Check if this mount belongs to our disk (device itself or any partition)
        if (mount_device == device_path ||
            (mount_device.starts_with(device_path) && mount_device.length() > device_path.length()) ||
            mount_device.find(device_name) != std::string_view::npos) {
            mount_points.emplace_back(entry->mnt_dir);
        }
    }
    endmntent(mtab);

    if (mount_points.empty()) {
        return true;  // Nothing to unmount
    }

    // Unmount in reverse order to handle nested mounts
    std::ranges::reverse(mount_points);

    bool all_success = true;
    for (const auto& mount_point : mount_points) {
        // Try regular unmount first
        if (umount(mount_point.c_str()) == 0) {
            std::cerr << std::format("INFO:Unmounted {}\n", mount_point);
            continue;
        }

        // Try force unmount
        if (umount2(mount_point.c_str(), MNT_FORCE) == 0) {
            std::cerr << std::format("INFO:Force unmounted {}\n", mount_point);
            continue;
        }

        // Try lazy unmount as last resort
        if (umount2(mount_point.c_str(), MNT_DETACH) == 0) {
            std::cerr << std::format("INFO:Lazy unmounted {}\n", mount_point);
            continue;
        }

        // All attempts failed
        report_error(std::format("Failed to unmount {}: {}", mount_point, strerror(errno)));
        all_success = false;
    }

    return all_success;
}

void fill_buffer_zeros(std::vector<uint8_t>& buffer) {
    std::fill(buffer.begin(), buffer.end(), 0x00);
}

void fill_buffer_ones(std::vector<uint8_t>& buffer) {
    std::fill(buffer.begin(), buffer.end(), 0xFF);
}

void fill_buffer_random(std::vector<uint8_t>& buffer, std::mt19937_64& rng) {
    auto* ptr = reinterpret_cast<uint64_t*>(buffer.data());
    const size_t count = buffer.size() / sizeof(uint64_t);
    for (size_t i = 0; i < count; ++i) {
        ptr[i] = rng();
    }
}

auto write_pass(int fd, uint64_t device_size, std::vector<uint8_t>& buffer,
                int current_pass, int total_passes) -> bool {
    uint64_t bytes_written = 0;
    auto last_report = std::chrono::steady_clock::now();

    // Seek to beginning
    if (lseek64(fd, 0, SEEK_SET) != 0) {
        report_error("Failed to seek to beginning of device");
        return false;
    }

    while (bytes_written < device_size) {
        if (g_cancelled) {
            return false;
        }

        const size_t to_write = std::min(buffer.size(),
                                         static_cast<size_t>(device_size - bytes_written));

        ssize_t written = write(fd, buffer.data(), to_write);
        if (written <= 0) {
            if (errno == ENOSPC) {
                // Reached end of device
                break;
            }
            report_error(std::format("Write failed: {}", strerror(errno)));
            return false;
        }

        bytes_written += static_cast<uint64_t>(written);

        // Report progress every 500ms
        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::milliseconds(500)) {
            double percentage = (static_cast<double>(bytes_written) /
                               static_cast<double>(device_size)) * 100.0;
            report_progress(current_pass, total_passes, percentage);
            last_report = now;
        }
    }

    // Sync to ensure data is written to disk
    fsync(fd);

    // Report 100% for this pass
    report_progress(current_pass, total_passes, 100.0);

    return true;
}

auto run_zero_fill(int fd, uint64_t size, std::vector<uint8_t>& buffer) -> bool {
    fill_buffer_zeros(buffer);
    return write_pass(fd, size, buffer, 1, 1);
}

auto run_random_fill(int fd, uint64_t size, std::vector<uint8_t>& buffer,
                     std::mt19937_64& rng) -> bool {
    fill_buffer_random(buffer, rng);
    return write_pass(fd, size, buffer, 1, 1);
}

auto run_dod_5220_22_m(int fd, uint64_t size, std::vector<uint8_t>& buffer,
                       std::mt19937_64& rng) -> bool {
    // Pass 1: Write zeros
    fill_buffer_zeros(buffer);
    if (!write_pass(fd, size, buffer, 1, 3)) return false;

    // Pass 2: Write ones
    fill_buffer_ones(buffer);
    if (!write_pass(fd, size, buffer, 2, 3)) return false;

    // Pass 3: Write random
    fill_buffer_random(buffer, rng);
    return write_pass(fd, size, buffer, 3, 3);
}

auto run_schneier(int fd, uint64_t size, std::vector<uint8_t>& buffer,
                  std::mt19937_64& rng) -> bool {
    // Pass 1: Write ones
    fill_buffer_ones(buffer);
    if (!write_pass(fd, size, buffer, 1, 7)) return false;

    // Pass 2: Write zeros
    fill_buffer_zeros(buffer);
    if (!write_pass(fd, size, buffer, 2, 7)) return false;

    // Passes 3-7: Write random
    for (int pass = 3; pass <= 7; ++pass) {
        fill_buffer_random(buffer, rng);
        if (!write_pass(fd, size, buffer, pass, 7)) return false;
    }

    return true;
}

auto run_vsitr(int fd, uint64_t size, std::vector<uint8_t>& buffer,
               std::mt19937_64& rng) -> bool {
    // Passes 1-6: Alternating zeros and ones
    for (int pass = 1; pass <= 6; ++pass) {
        if (pass % 2 == 1) {
            fill_buffer_zeros(buffer);
        } else {
            fill_buffer_ones(buffer);
        }
        if (!write_pass(fd, size, buffer, pass, 7)) return false;
    }

    // Pass 7: Random
    fill_buffer_random(buffer, rng);
    return write_pass(fd, size, buffer, 7, 7);
}

auto run_gost(int fd, uint64_t size, std::vector<uint8_t>& buffer,
              std::mt19937_64& rng) -> bool {
    // Pass 1: Write zeros
    fill_buffer_zeros(buffer);
    if (!write_pass(fd, size, buffer, 1, 2)) return false;

    // Pass 2: Write random
    fill_buffer_random(buffer, rng);
    return write_pass(fd, size, buffer, 2, 2);
}

auto run_gutmann(int fd, uint64_t size, std::vector<uint8_t>& buffer,
                 std::mt19937_64& rng) -> bool {
    constexpr int TOTAL_PASSES = 35;

    // Gutmann patterns for passes 5-31
    constexpr uint8_t GUTMANN_PATTERNS[][3] = {
        {0x55, 0x55, 0x55}, {0xAA, 0xAA, 0xAA}, {0x92, 0x49, 0x24},
        {0x49, 0x24, 0x92}, {0x24, 0x92, 0x49}, {0x00, 0x00, 0x00},
        {0x11, 0x11, 0x11}, {0x22, 0x22, 0x22}, {0x33, 0x33, 0x33},
        {0x44, 0x44, 0x44}, {0x55, 0x55, 0x55}, {0x66, 0x66, 0x66},
        {0x77, 0x77, 0x77}, {0x88, 0x88, 0x88}, {0x99, 0x99, 0x99},
        {0xAA, 0xAA, 0xAA}, {0xBB, 0xBB, 0xBB}, {0xCC, 0xCC, 0xCC},
        {0xDD, 0xDD, 0xDD}, {0xEE, 0xEE, 0xEE}, {0xFF, 0xFF, 0xFF},
        {0x92, 0x49, 0x24}, {0x49, 0x24, 0x92}, {0x24, 0x92, 0x49},
        {0x6D, 0xB6, 0xDB}, {0xB6, 0xDB, 0x6D}, {0xDB, 0x6D, 0xB6}
    };

    // Passes 1-4: Random
    for (int pass = 1; pass <= 4; ++pass) {
        fill_buffer_random(buffer, rng);
        if (!write_pass(fd, size, buffer, pass, TOTAL_PASSES)) return false;
    }

    // Passes 5-31: Specific patterns
    for (int pass = 5; pass <= 31; ++pass) {
        const auto& pattern = GUTMANN_PATTERNS[pass - 5];
        for (size_t i = 0; i < buffer.size(); i += 3) {
            buffer[i] = pattern[0];
            if (i + 1 < buffer.size()) buffer[i + 1] = pattern[1];
            if (i + 2 < buffer.size()) buffer[i + 2] = pattern[2];
        }
        if (!write_pass(fd, size, buffer, pass, TOTAL_PASSES)) return false;
    }

    // Passes 32-35: Random
    for (int pass = 32; pass <= 35; ++pass) {
        fill_buffer_random(buffer, rng);
        if (!write_pass(fd, size, buffer, pass, TOTAL_PASSES)) return false;
    }

    return true;
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " --device <path> --algorithm <algo> [--unmount]\n";
    std::cerr << "\nOptions:\n";
    std::cerr << "  --device <path>     Device to wipe (e.g., /dev/sda)\n";
    std::cerr << "  --algorithm <algo>  Wipe algorithm to use\n";
    std::cerr << "  --unmount           Unmount all partitions before wiping\n";
    std::cerr << "\nAlgorithms:\n";
    for (const auto& info : ALGORITHMS) {
        std::cerr << "  " << info.name << " (" << info.passes << " passes)\n";
    }
}

auto parse_algorithm(std::string_view name) -> std::optional<Algorithm> {
    for (const auto& info : ALGORITHMS) {
        if (info.name == name) {
            return info.algo;
        }
    }
    return std::nullopt;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    // Set up signal handlers for graceful cancellation
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Parse command line arguments
    std::string device_path;
    std::string algorithm_name;
    bool do_unmount = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--device" && i + 1 < argc) {
            device_path = argv[++i];
        } else if (arg == "--algorithm" && i + 1 < argc) {
            algorithm_name = argv[++i];
        } else if (arg == "--unmount") {
            do_unmount = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (device_path.empty() || algorithm_name.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Validate device path (security check)
    if (!validate_device_path(device_path)) {
        report_error("Invalid device path. Only /dev/sd*, /dev/nvme*, /dev/mmcblk*, /dev/vd* allowed");
        report_complete("error", "Invalid device path");
        return 1;
    }

    // Parse algorithm
    auto algo = parse_algorithm(algorithm_name);
    if (!algo) {
        report_error(std::format("Unknown algorithm: {}", algorithm_name));
        report_complete("error", "Unknown algorithm");
        return 1;
    }

    // Check we're running as root
    if (geteuid() != 0) {
        report_error("This helper must run as root");
        report_complete("error", "Not running as root");
        return 1;
    }

    // Unmount device if requested (must happen before opening for write)
    if (do_unmount) {
        if (!unmount_device(device_path)) {
            report_error("Failed to unmount all partitions. Close any applications using the disk.");
            report_complete("error", "Unmount failed");
            return 1;
        }
    }

    // Open device
    int fd = open(device_path.c_str(), O_WRONLY | O_SYNC | O_LARGEFILE);
    if (fd < 0) {
        report_error(std::format("Failed to open device: {}", strerror(errno)));
        report_complete("error", "Failed to open device");
        return 1;
    }

    // Get device size
    uint64_t device_size = get_device_size(fd);
    if (device_size == 0) {
        close(fd);
        report_error("Failed to get device size or device is empty");
        report_complete("error", "Invalid device size");
        return 1;
    }

    // Allocate buffer
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    // Initialize random number generator
    std::random_device rd;
    std::mt19937_64 rng(rd());

    // Run the wipe algorithm
    bool success = false;
    switch (*algo) {
        case Algorithm::ZERO_FILL:
            success = run_zero_fill(fd, device_size, buffer);
            break;
        case Algorithm::RANDOM_FILL:
            success = run_random_fill(fd, device_size, buffer, rng);
            break;
        case Algorithm::DOD_5220_22_M:
            success = run_dod_5220_22_m(fd, device_size, buffer, rng);
            break;
        case Algorithm::SCHNEIER:
            success = run_schneier(fd, device_size, buffer, rng);
            break;
        case Algorithm::VSITR:
            success = run_vsitr(fd, device_size, buffer, rng);
            break;
        case Algorithm::GOST_R_50739_95:
            success = run_gost(fd, device_size, buffer, rng);
            break;
        case Algorithm::GUTMANN:
            success = run_gutmann(fd, device_size, buffer, rng);
            break;
    }

    close(fd);

    if (g_cancelled) {
        report_complete("cancelled");
        return 2;
    } else if (success) {
        report_complete("success");
        return 0;
    } else {
        report_complete("error", "Wipe operation failed");
        return 1;
    }
}
