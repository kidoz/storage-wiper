/**
 * @file ProgressDisplay.cpp
 * @brief Terminal progress display implementation
 */

#include "cli/ProgressDisplay.hpp"

#include <unistd.h>

#include <cmath>
#include <format>
#include <iostream>

namespace cli {

namespace {

// ANSI color codes
constexpr auto RESET = "\033[0m";
constexpr auto BOLD = "\033[1m";
constexpr auto GREEN = "\033[32m";
constexpr auto RED = "\033[31m";
constexpr auto YELLOW = "\033[33m";
constexpr auto CYAN = "\033[36m";

}  // namespace

ProgressDisplay::ProgressDisplay(std::string device_path, std::string device_model,
                                 uint64_t device_size_bytes, std::string algorithm_name,
                                 int total_passes)
    : device_path_(std::move(device_path)), device_model_(std::move(device_model)),
      device_size_bytes_(device_size_bytes), algorithm_name_(std::move(algorithm_name)),
      total_passes_(total_passes), start_time_(std::chrono::steady_clock::now()) {

    // Disable colors if not a terminal
    color_enabled_ = is_terminal();
}

auto ProgressDisplay::is_terminal() -> bool {
    return isatty(STDOUT_FILENO) != 0;
}

void ProgressDisplay::set_color_enabled(bool enable) {
    color_enabled_ = enable;
}

void ProgressDisplay::update(const WipeProgress& progress) {
    // Print header on first update
    if (!header_printed_) {
        std::cout << "\n";
        if (color_enabled_) {
            std::cout << BOLD;
        }
        std::cout << "Wiping " << device_path_;
        if (!device_model_.empty()) {
            std::cout << " (" << device_model_ << ", " << format_bytes(device_size_bytes_) << ")";
        }
        std::cout << "\n";
        std::cout << "Algorithm: " << algorithm_name_ << " (" << total_passes_ << " pass"
                  << (total_passes_ != 1 ? "es" : "") << ")\n";
        if (color_enabled_) {
            std::cout << RESET;
        }
        std::cout << std::flush;
        header_printed_ = true;
    }

    // Generate progress bar
    std::string bar = generate_progress_bar(progress.percentage);

    // Format status line
    std::string status_line;

    if (progress.verification_in_progress) {
        // Verification phase
        status_line =
            std::format("Verifying: {} {:5.1f}%", bar, progress.verification_percentage);
    } else {
        // Wipe phase
        status_line = std::format("Pass {}/{}: {} {:5.1f}%", progress.current_pass,
                                  progress.total_passes, bar, progress.percentage);
    }

    // Add speed
    if (progress.speed_bytes_per_sec > 0) {
        status_line += "  |  " + format_speed(progress.speed_bytes_per_sec);
    }

    // Add ETA
    if (progress.estimated_seconds_remaining > 0) {
        status_line += "  |  ETA: " + format_duration(progress.estimated_seconds_remaining);
    }

    // Clear line and print
    clear_line();
    std::cout << status_line << std::flush;
}

void ProgressDisplay::complete(bool success, const std::string& message) {
    clear_line();
    std::cout << "\n";

    if (color_enabled_) {
        std::cout << (success ? GREEN : RED) << BOLD;
    }

    std::cout << (success ? "[OK] " : "[FAILED] ") << message;

    if (color_enabled_) {
        std::cout << RESET;
    }

    std::cout << "\n" << std::endl;
}

auto ProgressDisplay::format_bytes(uint64_t bytes) -> std::string {
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = KB * 1024;
    constexpr uint64_t GB = MB * 1024;
    constexpr uint64_t TB = GB * 1024;

    if (bytes >= TB) {
        return std::format("{:.1f} TB", static_cast<double>(bytes) / static_cast<double>(TB));
    } else if (bytes >= GB) {
        return std::format("{:.1f} GB", static_cast<double>(bytes) / static_cast<double>(GB));
    } else if (bytes >= MB) {
        return std::format("{:.1f} MB", static_cast<double>(bytes) / static_cast<double>(MB));
    } else if (bytes >= KB) {
        return std::format("{:.1f} KB", static_cast<double>(bytes) / static_cast<double>(KB));
    }
    return std::format("{} B", bytes);
}

auto ProgressDisplay::format_speed(uint64_t bytes_per_sec) -> std::string {
    return format_bytes(bytes_per_sec) + "/s";
}

auto ProgressDisplay::format_duration(int64_t seconds) -> std::string {
    if (seconds < 0) {
        return "--:--";
    }

    int64_t hours = seconds / 3600;
    int64_t minutes = (seconds % 3600) / 60;
    int64_t secs = seconds % 60;

    if (hours > 0) {
        return std::format("{}:{:02d}:{:02d}", hours, minutes, secs);
    }
    return std::format("{:02d}:{:02d}", minutes, secs);
}

auto ProgressDisplay::generate_progress_bar(double percentage) -> std::string {
    int filled = static_cast<int>(std::round(percentage / 100.0 * BAR_WIDTH));
    filled = std::clamp(filled, 0, BAR_WIDTH);

    std::string bar = "[";

    if (color_enabled_) {
        bar += GREEN;
    }

    for (int i = 0; i < filled; ++i) {
        bar += "\u2588";  // Full block character
    }

    if (color_enabled_) {
        bar += RESET;
    }

    for (int i = filled; i < BAR_WIDTH; ++i) {
        bar += "\u2591";  // Light shade character
    }

    bar += "]";

    return bar;
}

void ProgressDisplay::clear_line() {
    if (is_terminal()) {
        // Move cursor to beginning of line and clear
        std::cout << "\r\033[K";
    } else {
        // For non-terminals, just print newline
        std::cout << "\n";
    }
}

}  // namespace cli
