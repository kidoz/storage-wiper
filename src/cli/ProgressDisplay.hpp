/**
 * @file ProgressDisplay.hpp
 * @brief Terminal progress display for CLI wipe operations
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace cli {

/**
 * @class ProgressDisplay
 * @brief ANSI terminal progress bar display
 *
 * Displays a progress bar with percentage, speed, and ETA in the terminal.
 * Uses ANSI escape codes for cursor control and formatting.
 */
class ProgressDisplay {
public:
    /**
     * @brief Construct a progress display
     * @param device_path Device being wiped (for display)
     * @param device_model Device model name
     * @param device_size_bytes Device size in bytes
     * @param algorithm_name Name of wipe algorithm
     * @param total_passes Total number of passes
     */
    ProgressDisplay(std::string device_path, std::string device_model, uint64_t device_size_bytes,
                    std::string algorithm_name, int total_passes);

    /**
     * @brief Update the progress display
     * @param progress Current progress information
     */
    void update(const WipeProgress& progress);

    /**
     * @brief Mark the operation as complete
     * @param success Whether the operation succeeded
     * @param message Final status message
     */
    void complete(bool success, const std::string& message);

    /**
     * @brief Enable or disable ANSI color output
     * @param enable Whether to use colors
     */
    void set_color_enabled(bool enable);

    /**
     * @brief Check if terminal supports ANSI codes
     * @return true if terminal supports ANSI
     */
    [[nodiscard]] static auto is_terminal() -> bool;

private:
    /**
     * @brief Format bytes as human-readable string (e.g., "245 MB/s")
     */
    [[nodiscard]] static auto format_bytes(uint64_t bytes) -> std::string;

    /**
     * @brief Format speed as human-readable string
     */
    [[nodiscard]] static auto format_speed(uint64_t bytes_per_sec) -> std::string;

    /**
     * @brief Format duration as human-readable string (e.g., "12:34")
     */
    [[nodiscard]] static auto format_duration(int64_t seconds) -> std::string;

    /**
     * @brief Generate progress bar string
     */
    [[nodiscard]] auto generate_progress_bar(double percentage) -> std::string;

    /**
     * @brief Clear current line and move cursor to beginning
     */
    void clear_line();

    std::string device_path_;
    std::string device_model_;
    uint64_t device_size_bytes_;
    std::string algorithm_name_;
    int total_passes_;
    bool color_enabled_ = true;
    bool header_printed_ = false;
    std::chrono::steady_clock::time_point start_time_;

    static constexpr int BAR_WIDTH = 30;
};

}  // namespace cli
