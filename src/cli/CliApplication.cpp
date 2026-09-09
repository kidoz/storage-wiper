/**
 * @file CliApplication.cpp
 * @brief CLI application implementation
 */

#include "cli/CliApplication.hpp"

#include "cli/ProgressDisplay.hpp"
#include "services/DBusClient.hpp"
#include "util/JsonEscape.hpp"
#include "util/Logger.hpp"
#include "util/WipeCertificate.hpp"

#include <system_error>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <future>
#include <iomanip>
#include <iostream>
#include <thread>

#include "config.h"
#include <getopt.h>

namespace cli {

namespace {

// Global for signal handling.
// Writes to std::cerr (or anything that may take a lock or allocate) are not
// async-signal-safe, so the handler only flips the flag; the main loop owns
// any user-visible reporting.
std::atomic<bool> g_cancel_requested{false};

void signal_handler(int /*signal*/) noexcept {
    g_cancel_requested.store(true, std::memory_order_relaxed);
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
    {  "certificate", required_argument, nullptr, 'c'},
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
    while ((opt = getopt_long(argc, argv, "hVljw:a:vfyc:", long_options, nullptr)) != -1) {
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
            case 'c':
                options.certificate_path = optarg;
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
              << "  -l, --list                 List available disks\n"
              << "  -w, --wipe <device>        Wipe the specified device\n\n"
              << "Options:\n"
              << "  -h, --help                 Show this help message\n"
              << "  -V, --version              Show version information\n"
              << "  -j, --json                 Output in JSON format (with --list)\n"
              << "  -a, --algorithm <name>     Wipe algorithm (default: zero-fill)\n"
              << "  -v, --verify               Verify wipe by reading back data\n"
              << "  -f, --force-unmount        Unmount device before wiping\n"
              << "  -y, --yes                  Skip confirmation prompt\n"
              << "  -c, --certificate <path>   Write a wipe certificate after a successful\n"
              << "                             wipe: <path>.json and <path>.txt (a directory\n"
              << "                             receives an auto-named pair)\n\n"
              << "Algorithms (NIST SP 800-88 category in parentheses):\n"
              << "  zero-fill                  Single pass with zeros (Clear)\n"
              << "  random-fill                Single pass with random data (Clear)\n"
              << "  dod-5220-22-m              DoD 5220.22-M 3-pass standard (Clear)\n"
              << "  schneier                   Bruce Schneier 7-pass method (Clear)\n"
              << "  vsitr                      German VSITR 7-pass standard (Clear)\n"
              << "  gost                       Russian GOST R 50739-95 2-pass (Clear)\n"
              << "  gutmann                    Peter Gutmann 35-pass method (Clear)\n"
              << "  ata-secure-erase           Hardware/firmware secure erase (Purge):\n"
              << "                             ATA Security Erase for SATA, NVMe\n"
              << "                             Sanitize (crypto/block erase) for NVMe\n\n"
              << "Examples:\n"
              << "  " << APP_NAME << " --list\n"
              << "  " << APP_NAME << " --list --json\n"
              << "  " << APP_NAME << " --wipe /dev/sdb\n"
              << "  " << APP_NAME << " --wipe /dev/sdb --algorithm dod-5220-22-m --verify\n"
              << "  " << APP_NAME << " --wipe /dev/nvme0n1 --algorithm ata-secure-erase \\\n"
              << "      --certificate /tmp/wipe-report\n"
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

auto CliApplication::get_disks_blocking() -> std::expected<std::vector<DiskInfo>, util::Error> {
    std::promise<std::expected<std::vector<DiskInfo>, util::Error>> promise;
    auto future = promise.get_future();

    client_->get_available_disks([&promise](auto result) { promise.set_value(result); });

    auto* main_context = g_main_context_default();
    while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
        g_main_context_iteration(main_context, TRUE);
    }

    return future.get();
}

auto CliApplication::cmd_list(bool json) -> int {
    auto disks_res = get_disks_blocking();

    if (!disks_res) {
        if (json) {
            std::cout << "[]\n";
        } else {
            std::cerr << "Error listing disks: " << disks_res.error().message << "\n";
        }
        return 1;
    }

    const auto& disks = *disks_res;

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
    auto disks_res = get_disks_blocking();
    if (!disks_res) {
        std::cerr << "Error getting disk info: " << disks_res.error().message << "\n";
        return 1;
    }
    const auto& disks = *disks_res;

    auto disk_it = std::find_if(disks.begin(), disks.end(),
                                [&](const DiskInfo& d) { return d.path == options.device_path; });

    if (disk_it == disks.end()) {
        LOG_ERROR("CLI", std::format("Device not found: {}", options.device_path));
        std::cerr << "Error: Device not found: " << options.device_path << "\n";
        return 1;
    }

    const auto& disk = *disk_it;
    if (disk.is_partition && *algo == WipeAlgorithm::ATA_SECURE_ERASE) {
        std::cerr << "Error: Hardware secure erase cannot target a partition. "
                     "Select a whole disk or use an overwrite algorithm.\n";
        return 1;
    }

    // Scope statement: a partition wipe spares its siblings, a disk wipe
    // takes every partition and the partition table with it.
    std::string scope_note;
    if (disk.is_partition) {
        if (!disk.parent_disk.empty()) {
            scope_note = "Scope: only this partition is erased. Other partitions and the "
                         "partition table on " +
                         disk.parent_disk + " are not touched.";
        }
    } else {
        std::vector<std::string> children;
        for (const auto& candidate : disks) {
            if (candidate.is_partition && candidate.parent_disk == disk.path) {
                children.push_back(candidate.path);
            }
        }
        if (!children.empty()) {
            scope_note = "Scope: the whole device is erased, including its partitions (";
            for (size_t i = 0; i < children.size(); ++i) {
                scope_note += children[i];
                if (i + 1 < children.size()) {
                    scope_note += ", ";
                }
            }
            scope_note += ") and the partition table.";
        }
    }

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
        if (!confirm_wipe(options.device_path, options.algorithm, scope_note)) {
            std::cout << "Aborted.\n";
            return 1;
        }
    }

    // Warn early when verification cannot be honored for this algorithm, so
    // the user is not left assuming a verification step that never runs.
    if (options.verify && !client_->supports_verification(*algo)) {
        std::cerr << "Warning: algorithm '" << options.algorithm
                  << "' does not support post-wipe verification; --verify will be ignored.\n";
        LOG_WARNING("CLI",
                    std::format("Verification requested for {} with algorithm {} which does not "
                                "support it; ignoring",
                                options.device_path, options.algorithm));
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
    std::atomic<uint64_t> peak_speed{0};
    std::atomic<bool> verification_enabled_reported{false};
    std::atomic<bool> verification_passed{false};
    std::atomic<uint64_t> bad_blocks_total{0};
    std::string final_message;

    const auto start_wall = std::chrono::system_clock::now();
    const auto start_steady = std::chrono::steady_clock::now();

    // Progress callback
    auto callback = [&](const WipeProgress& p) {
        peak_speed.store(std::max(peak_speed.load(), p.speed_bytes_per_sec));
        bad_blocks_total.store(p.bad_block_count);
        if (p.is_complete) {
            complete.store(true);
            success.store(!p.has_error);
            verification_enabled_reported.store(p.verification_enabled);
            verification_passed.store(p.verification_passed);
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
    bool cancel_reported = false;
    while (!complete.load()) {
        // Process GLib events for D-Bus signals
        g_main_context_iteration(main_context, FALSE);

        if (g_cancel_requested.load(std::memory_order_relaxed)) {
            if (!cancel_reported) {
                std::cerr << "\nCancellation requested...\n";
                cancel_reported = true;
            }
            client_->cancel_operation(options.device_path);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    progress.complete(success.load(), final_message);

    // Write the wipe certificate on success
    if (success.load() && !options.certificate_path.empty()) {
        util::WipeCertificateData data{};
        data.device_path = disk.path;
        data.model = disk.model;
        data.serial = disk.serial;
        data.size_bytes = disk.size_bytes;
        data.algorithm_name = client_->get_algorithm_name(*algo);
        data.nist_category = client_->get_nist_category(*algo);
        data.total_passes = client_->get_pass_count(*algo);
        data.started_at = util::iso8601_utc(start_wall);
        data.completed_at = util::iso8601_utc(std::chrono::system_clock::now());
        data.duration_seconds =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now() - start_steady)
                                      .count());
        data.peak_speed_bytes_per_sec = peak_speed.load();
        data.verification_enabled = verification_enabled_reported.load();
        data.verification_passed = verification_passed.load();
        data.is_partition = disk.is_partition;
        data.parent_disk = disk.parent_disk;
        data.bad_block_count = bad_blocks_total.load();
        data.success = true;
        data.tool_version = PROJECT_VERSION;

        namespace fs = std::filesystem;
        const fs::path requested{options.certificate_path};
        // The throwing overload would abort the run after the wipe already
        // succeeded, for example on a path the process cannot stat.
        std::error_code ec;
        const bool to_directory =
            options.certificate_path.ends_with('/') || fs::is_directory(requested, ec);
        auto written = to_directory ? util::write_wipe_certificate(requested, data)
                                    : util::write_certificate_to_base(requested, data);

        if (written) {
            std::cout << "Certificate written: " << written->string() << ".json, "
                      << written->string() << ".txt\n";
        } else {
            LOG_ERROR("CLI", std::format("Certificate write failed: {}", written.error()));
            std::cerr << "Warning: certificate could not be written: " << written.error() << "\n";
        }
    }

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
    if (lower == "ata-secure-erase" || lower == "hardware-secure-erase") {
        return WipeAlgorithm::ATA_SECURE_ERASE;
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

auto CliApplication::confirm_wipe(const std::string& device_path, const std::string& algorithm,
                                  const std::string& scope_note) -> bool {
    std::cout << "\n";
    std::cout << "\033[1;31mWARNING: This will PERMANENTLY DESTROY all data on " << device_path
              << "!\033[0m\n";
    std::cout << "Algorithm: " << algorithm << "\n";
    if (algorithm == "ata-secure-erase" && device_path.starts_with("/dev/nvme")) {
        // Sanitize and Format are controller-scoped, so sibling namespaces go too.
        std::cout << "\033[1;31mAn NVMe firmware erase is issued to the controller and erases "
                     "EVERY namespace on it, not only "
                  << device_path << ".\033[0m\n";
    }
    if (!scope_note.empty()) {
        std::cout << scope_note << "\n";
    }
    std::cout << "\n";
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
        std::cout << "    \"path\": \"" << util::json_escape(disk.path) << "\",\n";
        std::cout << "    \"model\": \"" << util::json_escape(disk.model) << "\",\n";
        std::cout << "    \"size_bytes\": " << disk.size_bytes << ",\n";
        std::cout << "    \"is_ssd\": " << (disk.is_ssd ? "true" : "false") << ",\n";
        std::cout << "    \"is_removable\": " << (disk.is_removable ? "true" : "false") << ",\n";
        std::cout << "    \"is_mounted\": " << (disk.is_mounted ? "true" : "false") << ",\n";
        std::cout << "    \"mount_point\": \"" << util::json_escape(disk.mount_point) << "\",\n";
        std::cout << "    \"filesystem\": \"" << util::json_escape(disk.filesystem) << "\",\n";
        std::cout << "    \"is_partition\": " << (disk.is_partition ? "true" : "false") << ",\n";
        std::cout << "    \"parent_disk\": \"" << util::json_escape(disk.parent_disk) << "\",\n";
        std::cout << "    \"smart_status\": \"" << util::json_escape(disk.smart.status_string())
                  << "\",\n";
        std::cout << "    \"smart\": {\n";
        std::cout << "      \"available\": " << (disk.smart.available ? "true" : "false") << ",\n";
        std::cout << "      \"healthy\": " << (disk.smart.healthy ? "true" : "false") << ",\n";

        // Unknown attributes are reported as null rather than the -1 sentinel
        auto print_attribute = [](const char* name, int64_t value, bool last) {
            std::cout << "      \"" << name << "\": ";
            if (value < 0) {
                std::cout << "null";
            } else {
                std::cout << value;
            }
            std::cout << (last ? "\n" : ",\n");
        };

        print_attribute("power_on_hours", disk.smart.power_on_hours, false);
        print_attribute("temperature_celsius", disk.smart.temperature_celsius, false);
        print_attribute("reallocated_sectors", disk.smart.reallocated_sectors, false);
        print_attribute("pending_sectors", disk.smart.pending_sectors, false);
        print_attribute("uncorrectable_errors", disk.smart.uncorrectable_errors, false);
        print_attribute("percentage_used", disk.smart.percentage_used, false);
        print_attribute("available_spare_percent", disk.smart.available_spare_percent, false);
        print_attribute("available_spare_threshold_percent",
                        disk.smart.available_spare_threshold_percent, true);
        std::cout << "    }\n";
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
        if (disk.is_partition) {
            type += " Part";
        }
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
