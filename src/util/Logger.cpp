/**
 * @file Logger.cpp
 * @brief Thread-safe logging utility implementation
 */

#include "util/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace util {

Logger::~Logger() {
    shutdown();
}

auto Logger::instance() -> Logger& {
    static Logger instance;
    return instance;
}

auto Logger::initialize(const std::filesystem::path& log_dir, const std::string& app_name,
                        LogLevel min_level, LogRotationPolicy policy) -> bool {
    std::lock_guard lock(mutex_);

    // Close existing file if reinitializing
    if (file_.is_open()) {
        file_.close();
    }

    log_dir_ = log_dir;
    app_name_ = app_name;
    min_level_ = min_level;
    policy_ = policy;
    current_file_size_ = 0;

    // Create log directory if it doesn't exist
    std::error_code ec;
    if (!std::filesystem::exists(log_dir_)) {
        if (!std::filesystem::create_directories(log_dir_, ec)) {
            std::cerr << "Logger: Failed to create log directory: " << log_dir_
                      << " - " << ec.message() << std::endl;
            return false;
        }
    }

    // Open log file
    if (!open_log_file()) {
        return false;
    }

    initialized_ = true;

    // Log initialization
    file_ << get_timestamp() << " [INFO ] [Logger] Logger initialized: "
          << "app=" << app_name_
          << " dir=" << log_dir_.string()
          << " level=" << level_to_string(min_level_)
          << " max_size=" << policy_.max_file_size_bytes
          << " max_files=" << policy_.max_files
          << std::endl;
    file_.flush();

    return true;
}

auto Logger::is_initialized() const -> bool {
    std::lock_guard lock(mutex_);
    return initialized_;
}

auto Logger::open_log_file() -> bool {
    auto log_path = log_dir_ / (app_name_ + ".log");

    file_.open(log_path, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "Logger: Failed to open log file: " << log_path << std::endl;
        return false;
    }

    // Get current file size
    std::error_code ec;
    current_file_size_ = std::filesystem::file_size(log_path, ec);
    if (ec) {
        current_file_size_ = 0;
    }

    return true;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
    std::lock_guard lock(mutex_);

    // Skip if below minimum level
    if (level < min_level_) {
        return;
    }

    // Format log line
    auto timestamp = get_timestamp();
    auto level_str = level_to_string(level);

    std::string log_line = std::format("{}[{}] [{}] {}\n", timestamp, level_str, component, message);

    // Write to file if initialized
    if (initialized_ && file_.is_open()) {
        check_and_rotate();
        file_ << log_line;
        file_.flush();
        current_file_size_ += log_line.size();
    }

    // Write to console if enabled
    if (console_output_) {
        std::cerr << log_line;
    }
}

void Logger::debug(std::string_view component, std::string_view message) {
    log(LogLevel::DEBUG, component, message);
}

void Logger::info(std::string_view component, std::string_view message) {
    log(LogLevel::INFO, component, message);
}

void Logger::warning(std::string_view component, std::string_view message) {
    log(LogLevel::WARNING, component, message);
}

void Logger::error(std::string_view component, std::string_view message) {
    log(LogLevel::ERROR, component, message);
}

void Logger::flush() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void Logger::set_min_level(LogLevel level) {
    std::lock_guard lock(mutex_);
    min_level_ = level;
}

auto Logger::get_min_level() const -> LogLevel {
    std::lock_guard lock(mutex_);
    return min_level_;
}

void Logger::set_console_output(bool enable) {
    std::lock_guard lock(mutex_);
    console_output_ = enable;
}

auto Logger::get_log_file_path() const -> std::filesystem::path {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return {};
    }
    return log_dir_ / (app_name_ + ".log");
}

void Logger::shutdown() {
    std::lock_guard lock(mutex_);

    if (initialized_ && file_.is_open()) {
        file_ << get_timestamp() << " [INFO ] [Logger] Logger shutting down" << std::endl;
        file_.flush();
        file_.close();
    }

    initialized_ = false;
}

auto Logger::get_timestamp() -> std::string {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    gmtime_r(&time_t_now, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "Z ";

    return oss.str();
}

auto Logger::level_to_string(LogLevel level) -> std::string_view {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO ";
        case LogLevel::WARNING:
            return "WARN ";
        case LogLevel::ERROR:
            return "ERROR";
    }
    return "?????";
}

void Logger::check_and_rotate() {
    if (current_file_size_ >= policy_.max_file_size_bytes) {
        rotate_logs();
    }
}

void Logger::rotate_logs() {
    // Close current file
    if (file_.is_open()) {
        file_.close();
    }

    auto base_path = log_dir_ / (app_name_ + ".log");
    std::error_code ec;

    // Delete oldest file if at max
    auto oldest_path = log_dir_ / std::format("{}.{}.log", app_name_, policy_.max_files);
    if (std::filesystem::exists(oldest_path)) {
        std::filesystem::remove(oldest_path, ec);
    }

    // Shift existing rotated files (n-1 -> n, n-2 -> n-1, etc.)
    for (int i = policy_.max_files - 1; i >= 1; --i) {
        auto old_path = log_dir_ / std::format("{}.{}.log", app_name_, i);
        auto new_path = log_dir_ / std::format("{}.{}.log", app_name_, i + 1);

        if (std::filesystem::exists(old_path)) {
            std::filesystem::rename(old_path, new_path, ec);
        }
    }

    // Rotate current file to .1.log
    auto first_rotated = log_dir_ / std::format("{}.1.log", app_name_);
    if (std::filesystem::exists(base_path)) {
        std::filesystem::rename(base_path, first_rotated, ec);
    }

    // Reopen fresh log file
    open_log_file();

    // Log rotation event
    file_ << get_timestamp() << " [INFO ] [Logger] Log file rotated" << std::endl;
    file_.flush();
}

}  // namespace util
