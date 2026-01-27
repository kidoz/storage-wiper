/**
 * @file CliApplication.cpp
 * @brief CLI application implementation
 */

#include "cli/CliApplication.hpp"

#include "cli/ProgressDisplay.hpp"
#include "config.h"
#include "services/DBusClient.hpp"
#include "util/Logger.hpp"

#include <gio/gio.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <thread>

#include <getopt.h>

namespace cli {

namespace {

// Global for signal handling
std::atomic<bool> g_cancel_requested{false};

void signal_handler(int /*signal*/) {
    g_cancel_requested.store(true);
    std::cerr << "\nCancellation requested..." << std::endl;
}

// Application name
constexpr auto APP_NAME = "storage-wiper-cli";

// Command line options
const struct option long_options[] = {
    {         "help",       no_argument, nullptr, 'h'},
    {      "version",       no_argument, nullptr, 'V'},
    {         "list",       no_argument, nullptr, 'l'},
    {         "json",       no_argument, nullptr, 'j'},
    {         "wipe", required_argument, nullptr, 'w'},
    {    "algorithm", required_argument, nullptr, 'a'},
    {       "verify",       no_argument, nullptr, 'v'},
    {"force-unmount",       no_argument, nullptr, 'f'},
    {          "yes",       no_argument, nullptr, 'y'},
    {        nullptr,                 0, nullptr,   0}
};

}  // namespace

CliApplication::CliApplication() = default;

CliApplication::~CliApplication() = default;

auto CliApplication::run(int argc, char* argv[]) -> int {
    // Initialize logger for CLI application
    auto log_dir = std::filesystem::path(g_get_user_data_dir()) / "storage-wiper" / "logs";
    util::Logger::instance().initialize(log_dir, "storage-wiper-cli");

    auto options = parse_args(argc, argv);

    if (options.show_help) {
        print_help();
        return 0;
    }

    if (options.show_version) {
        print_version();
        return 0;
    }

    // Connect to D-Bus helper
    if (!connect()) {
        LOG_ERROR("CLI", "Failed to connect to storage-wiper-helper service");
        std::cerr << "Error: Failed to connect to storage-wiper-helper service.\n"
                  << "Make sure the helper is installed and D-Bus is running.\n";
        return 1;
    }

    if (options.list_disks) {
        return cmd_list(options.json_output);
    }

    if (options.wipe) {
        return cmd_wipe(options);
    }

    // No command specified
    print_help();
    return 1;
}

auto CliApplication::parse_args(int argc, char* argv[]) -> CliOptions {
    CliOptions options;

    int opt;
    while ((opt = getopt_long(argc, argv, "hVljw:a:vfy", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                options.show_help = true;
                break;
            case 'V':
                options.show_version = true;
                break;
            case 'l':
                options.list_disks = true;
                break;
            case 'j':
                options.json_output = true;
                break;
            case 'w':
                options.wipe = true;
                options.device_path = optarg;
                break;
            case 'a':
                options.algorithm = optarg;
                break;
            case 'v':
                options.verify = true;
                break;
            case 'f':
                options.force_unmount = true;
                break;
            case 'y':
                options.no_confirm = true;
                break;
            default:
                options.show_help = true;
                break;
        }
    }

    return options;
}

void CliApplication::print_help() {
    std::cout << "Usage: " << APP_NAME << " [OPTIONS]\n\n"
              << "Secure disk wiping tool\n\n"
              << "Commands:\n"
              << "  -l, --list              List available disks\n"
              << "  -w, --wipe <device>     Wipe the specified device\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "  -V, --version           Show version information\n"
              << "  -j, --json              Output in JSON format (with --list)\n"
              << "  -a, --algorithm <name>  Wipe algorithm (default: zero-fill)\n"
              << "  -v, --verify            Verify wipe by reading back data\n"
              << "  -f, --force-unmount     Unmount device before wiping\n"
              << "  -y, --yes               Skip confirmation prompt\n\n"
              << "Algorithms:\n"
              << "  zero-fill               Single pass with zeros\n"
              << "  random-fill             Single pass with random data\n"
              << "  dod-5220-22-m           DoD 5220.22-M 3-pass standard\n"
              << "  schneier                Bruce Schneier 7-pass method\n"
              << "  vsitr                   German VSITR 7-pass standard\n"
              << "  gost                    Russian GOST R 50739-95 2-pass\n"
              << "  gutmann                 Peter Gutmann 35-pass method\n\n"
              << "Examples:\n"
              << "  " << APP_NAME << " --list\n"
              << "  " << APP_NAME << " --list --json\n"
              << "  " << APP_NAME << " --wipe /dev/sdb\n"
              << "  " << APP_NAME << " --wipe /dev/sdb --algorithm dod-5220-22-m --verify\n"
              << std::endl;
}

void CliApplication::print_version() {
    std::cout << APP_NAME << " version " << PROJECT_VERSION << "\n"
              << "Part of Storage Wiper - Secure disk wiping tool\n";
}

auto CliApplication::connect() -> bool {
    client_ = std::make_unique<DBusClient>();
    return client_->connect();
}

auto CliApplication::cmd_list(bool json) -> int {
    auto disks = client_->get_available_disks();

    if (disks.empty()) {
        if (json) {
            std::cout << "[]\n";
        } else {
            std::cout << "No disks found.\n";
        }
        return 0;
    }

    if (json) {
        print_disks_json(disks);
    } else {
        print_disks_table(disks);
    }

    return 0;
}

auto CliApplication::cmd_wipe(const CliOptions& options) -> int {
    // Parse algorithm
    auto algo = parse_algorithm(options.algorithm);
    if (!algo) {
        LOG_ERROR("CLI", std::format("Unknown algorithm: {}", options.algorithm));
        std::cerr << "Error: Unknown algorithm '" << options.algorithm << "'\n"
                  << "Run with --help to see available algorithms.\n";
        return 1;
    }

    // Validate device path
    auto valid = client_->validate_device_path(options.device_path);
    if (!valid) {
        LOG_ERROR("CLI", std::format("Invalid device path {}: {}", options.device_path,
                                     valid.error().message));
        std::cerr << "Error: " << valid.error().message << "\n";
        return 1;
    }

    // Get disk info
    auto disks = client_->get_available_disks();
    auto disk_it = std::find_if(disks.begin(), disks.end(),
                                [&](const DiskInfo& d) { return d.path == options.device_path; });

    if (disk_it == disks.end()) {
        LOG_ERROR("CLI", std::format("Device not found: {}", options.device_path));
        std::cerr << "Error: Device not found: " << options.device_path << "\n";
        return 1;
    }

    const auto& disk = *disk_it;

    // Check if mounted
    if (disk.is_mounted) {
        if (options.force_unmount) {
            std::cout << "Unmounting " << options.device_path << "...\n";
            auto unmount_result = client_->unmount_disk(options.device_path);
            if (!unmount_result) {
                LOG_ERROR("CLI", std::format("Failed to unmount {}: {}", options.device_path,
                                             unmount_result.error().message));
                std::cerr << "Error: Failed to unmount: " << unmount_result.error().message << "\n";
                return 1;
            }
        } else {
            std::cerr << "Error: Device is mounted at " << disk.mount_point << "\n"
                      << "Use --force-unmount to unmount before wiping.\n";
            return 1;
        }
    }

    // Confirm
    if (!options.no_confirm) {
        if (!confirm_wipe(options.device_path, options.algorithm)) {
            std::cout << "Aborted.\n";
            return 1;
        }
    }

    // Set up signal handler for graceful cancellation
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create progress display
    ProgressDisplay progress(options.device_path, disk.model, disk.size_bytes,
                             client_->get_algorithm_name(*algo), client_->get_pass_count(*algo));

    // Track completion
    std::atomic<bool> complete{false};
    std::atomic<bool> success{false};
    std::string final_message;

    // Progress callback
    auto callback = [&](const WipeProgress& p) {
        if (p.is_complete) {
            complete.store(true);
            success.store(!p.has_error);
            final_message = p.status;
            if (p.has_error && !p.error_message.empty()) {
                final_message = p.error_message;
            }
        } else {
            progress.update(p);
        }
    };

    // Start wipe
    if (!client_->wipe_disk(options.device_path, *algo, callback, options.verify)) {
        LOG_ERROR("CLI", std::format("Failed to start wipe operation for {}", options.device_path));
        std::cerr << "Error: Failed to start wipe operation.\n";
        return 1;
    }

    // Wait for completion, checking for cancellation
    auto main_context = g_main_context_default();
    while (!complete.load()) {
        // Process GLib events for D-Bus signals
        g_main_context_iteration(main_context, FALSE);

        if (g_cancel_requested.load()) {
            client_->cancel_current_operation();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    progress.complete(success.load(), final_message);

    return success.load() ? 0 : 1;
}

auto CliApplication::parse_algorithm(const std::string& name) -> std::optional<WipeAlgorithm> {
    // Convert to lowercase for comparison
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "zero-fill" || lower == "zero" || lower == "zerofill") {
        return WipeAlgorithm::ZERO_FILL;
    }
    if (lower == "random-fill" || lower == "random" || lower == "randomfill") {
        return WipeAlgorithm::RANDOM_FILL;
    }
    if (lower == "dod-5220-22-m" || lower == "dod" || lower == "dod522022m") {
        return WipeAlgorithm::DOD_5220_22_M;
    }
    if (lower == "schneier") {
        return WipeAlgorithm::SCHNEIER;
    }
    if (lower == "vsitr") {
        return WipeAlgorithm::VSITR;
    }
    if (lower == "gost" || lower == "gost-r-50739-95") {
        return WipeAlgorithm::GOST_R_50739_95;
    }
    if (lower == "gutmann") {
        return WipeAlgorithm::GUTMANN;
    }

    return std::nullopt;
}

auto CliApplication::algorithm_to_string(WipeAlgorithm algo) -> std::string {
    switch (algo) {
        case WipeAlgorithm::ZERO_FILL:
            return "zero-fill";
        case WipeAlgorithm::RANDOM_FILL:
            return "random-fill";
        case WipeAlgorithm::DOD_5220_22_M:
            return "dod-5220-22-m";
        case WipeAlgorithm::SCHNEIER:
            return "schneier";
        case WipeAlgorithm::VSITR:
            return "vsitr";
        case WipeAlgorithm::GOST_R_50739_95:
            return "gost";
        case WipeAlgorithm::GUTMANN:
            return "gutmann";
        case WipeAlgorithm::ATA_SECURE_ERASE:
            return "ata-secure-erase";
    }
    return "unknown";
}

auto CliApplication::confirm_wipe(const std::string& device_path, const std::string& algorithm)
    -> bool {
    std::cout << "\n";
    std::cout << "\033[1;31mWARNING: This will PERMANENTLY DESTROY all data on " << device_path
              << "!\033[0m\n";
    std::cout << "Algorithm: " << algorithm << "\n\n";
    std::cout << "Type 'yes' to confirm: ";
    std::cout.flush();

    std::string input;
    std::getline(std::cin, input);

    return input == "yes";
}

void CliApplication::print_disks_json(const std::vector<DiskInfo>& disks) {
    std::cout << "[\n";
    for (size_t i = 0; i < disks.size(); ++i) {
        const auto& disk = disks[i];

        std::cout << "  {\n";
        std::cout << "    \"path\": \"" << disk.path << "\",\n";
        std::cout << "    \"model\": \"" << disk.model << "\",\n";
        std::cout << "    \"size_bytes\": " << disk.size_bytes << ",\n";
        std::cout << "    \"is_ssd\": " << (disk.is_ssd ? "true" : "false") << ",\n";
        std::cout << "    \"is_removable\": " << (disk.is_removable ? "true" : "false") << ",\n";
        std::cout << "    \"is_mounted\": " << (disk.is_mounted ? "true" : "false") << ",\n";
        std::cout << "    \"mount_point\": \"" << disk.mount_point << "\",\n";
        std::cout << "    \"filesystem\": \"" << disk.filesystem << "\",\n";
        std::cout << "    \"smart_status\": \"" << disk.smart.status_string() << "\"\n";
        std::cout << "  }" << (i < disks.size() - 1 ? "," : "") << "\n";
    }
    std::cout << "]\n";
}

void CliApplication::print_disks_table(const std::vector<DiskInfo>& disks) {
    // Column widths for table formatting
    constexpr int COL_PATH = 15;
    constexpr int COL_MODEL = 30;
    constexpr int COL_SIZE = 12;
    constexpr int COL_TYPE = 8;
    constexpr int COL_STATUS = 12;
    constexpr int COL_HEALTH = 10;

    // Print header
    std::cout << std::left << std::setw(COL_PATH) << "DEVICE" << std::setw(COL_MODEL) << "MODEL"
              << std::setw(COL_SIZE) << "SIZE" << std::setw(COL_TYPE) << "TYPE"
              << std::setw(COL_STATUS) << "STATUS" << std::setw(COL_HEALTH) << "HEALTH"
              << "\n";
    std::cout << std::string(COL_PATH + COL_MODEL + COL_SIZE + COL_TYPE + COL_STATUS + COL_HEALTH,
                             '-')
              << "\n";

    // Print disks
    for (const auto& disk : disks) {
        // Format size
        auto format_size = [](uint64_t bytes) -> std::string {
            constexpr uint64_t GB = 1'024ULL * 1'024 * 1'024;
            constexpr uint64_t TB = GB * 1'024;

            if (bytes >= TB) {
                return std::format("{:.1f} TB",
                                   static_cast<double>(bytes) / static_cast<double>(TB));
            }
            return std::format("{:.1f} GB", static_cast<double>(bytes) / static_cast<double>(GB));
        };

        std::string type = disk.is_ssd ? "SSD" : "HDD";
        if (disk.is_removable) {
            type = "Removable";
        }

        std::string status = disk.is_mounted ? "Mounted" : "Available";

        // Truncate model if too long
        std::string model = disk.model;
        if (model.length() > COL_MODEL - 2) {
            model = model.substr(0, COL_MODEL - 5) + "...";
        }

        std::cout << std::left << std::setw(COL_PATH) << disk.path << std::setw(COL_MODEL) << model
                  << std::setw(COL_SIZE) << format_size(disk.size_bytes) << std::setw(COL_TYPE)
                  << type << std::setw(COL_STATUS) << status << std::setw(COL_HEALTH)
                  << disk.smart.status_string() << "\n";
    }
}

}  // namespace cli
