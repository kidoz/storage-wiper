/**
 * @file CliApplication.hpp
 * @brief CLI application for storage wiping
 */

#pragma once

#include "models/DiskInfo.hpp"
#include "models/WipeTypes.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class DBusClient;

namespace cli {

/**
 * @struct CliOptions
 * @brief Parsed command line options
 */
struct CliOptions {
    bool show_help = false;
    bool show_version = false;
    bool list_disks = false;
    bool json_output = false;
    bool wipe = false;
    std::string device_path;
    std::string algorithm = "zero-fill";
    bool verify = false;
    bool force_unmount = false;
    bool no_confirm = false;
};

/**
 * @class CliApplication
 * @brief Command-line application for disk wiping
 *
 * Provides command-line interface for:
 * - Listing available disks
 * - Wiping disks with various algorithms
 * - Optional verification after wipe
 */
class CliApplication {
public:
    CliApplication();
    ~CliApplication();

    // Non-copyable
    CliApplication(const CliApplication&) = delete;
    CliApplication& operator=(const CliApplication&) = delete;

    /**
     * @brief Run the CLI application
     * @param argc Argument count
     * @param argv Argument values
     * @return Exit code (0 = success)
     */
    auto run(int argc, char* argv[]) -> int;

    /**
     * @brief Parse command line arguments
     * @param argc Argument count
     * @param argv Argument values
     * @return Parsed options
     */
    [[nodiscard]] static auto parse_args(int argc, char* argv[]) -> CliOptions;

    /**
     * @brief Print help message
     */
    static void print_help();

    /**
     * @brief Print version information
     */
    static void print_version();

private:
    /**
     * @brief Connect to D-Bus helper
     * @return true if connected
     */
    [[nodiscard]] auto connect() -> bool;

    /**
     * @brief List available disks
     * @param json Output as JSON if true
     * @return Exit code
     */
    auto cmd_list(bool json) -> int;

    /**
     * @brief Wipe a disk
     * @param options Wipe options
     * @return Exit code
     */
    auto cmd_wipe(const CliOptions& options) -> int;

    /**
     * @brief Convert algorithm string to enum
     * @param name Algorithm name (e.g., "zero-fill", "dod-5220-22-m")
     * @return Algorithm enum, or nullopt if invalid
     */
    [[nodiscard]] static auto parse_algorithm(const std::string& name)
        -> std::optional<WipeAlgorithm>;

    /**
     * @brief Get algorithm name string for display
     */
    [[nodiscard]] static auto algorithm_to_string(WipeAlgorithm algo) -> std::string;

    /**
     * @brief Prompt user for confirmation
     * @param device_path Device to wipe
     * @param algorithm Algorithm name
     * @return true if user confirms
     */
    [[nodiscard]] static auto confirm_wipe(const std::string& device_path,
                                           const std::string& algorithm) -> bool;

    /**
     * @brief Print disk list as JSON
     */
    void print_disks_json(const std::vector<DiskInfo>& disks);

    /**
     * @brief Print disk list as table
     */
    void print_disks_table(const std::vector<DiskInfo>& disks);

    std::unique_ptr<DBusClient> client_;
};

}  // namespace cli
