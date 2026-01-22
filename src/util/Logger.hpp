/**
 * @file Logger.hpp
 * @brief Thread-safe logging utility with file rotation
 *
 * Provides structured logging with ISO 8601 timestamps, log levels,
 * component tags, and automatic file rotation.
 */

#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace util {

/**
 * @enum LogLevel
 * @brief Log severity levels
 */
enum class LogLevel {
    DEBUG,    ///< Detailed debugging information
    INFO,     ///< General operational information
    WARNING,  ///< Warning conditions
    ERROR     ///< Error conditions
};

/**
 * @struct LogRotationPolicy
 * @brief Configuration for log file rotation
 */
struct LogRotationPolicy {
    size_t max_file_size_bytes = 10 * 1024 * 1024;  ///< Max size before rotation (10MB default)
    int max_files = 7;                               ///< Number of rotated files to keep
    bool compress_rotated = false;                   ///< Whether to compress rotated files (future)
};

/**
 * @class Logger
 * @brief Thread-safe singleton logger with file output and rotation
 *
 * Usage:
 * @code
 * auto& log = util::Logger::instance();
 * log.initialize("/var/log/storage-wiper", "storage-wiper-helper");
 * log.info("WipeService", "Starting wipe operation");
 * log.error("DiskService", std::format("Failed to open device: {}", path));
 * @endcode
 */
class Logger {
public:
    /**
     * @brief Get the singleton logger instance
     * @return Reference to the global logger
     */
    static auto instance() -> Logger&;

    // Prevent copying
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /**
     * @brief Initialize the logger with output directory and application name
     * @param log_dir Directory for log files (created if doesn't exist)
     * @param app_name Application name used in log filename
     * @param min_level Minimum level to log (default: INFO)
     * @param policy Rotation policy (default: 10MB, 7 files)
     * @return true if initialized successfully
     *
     * Log files are named: {app_name}.log
     * Rotated files: {app_name}.1.log, {app_name}.2.log, etc.
     */
    auto initialize(const std::filesystem::path& log_dir, const std::string& app_name,
                    LogLevel min_level = LogLevel::INFO,
                    LogRotationPolicy policy = {}) -> bool;

    /**
     * @brief Check if logger is initialized
     * @return true if initialized and ready for logging
     */
    [[nodiscard]] auto is_initialized() const -> bool;

    /**
     * @brief Log a message with specified level and component
     * @param level Log level
     * @param component Component/module name (e.g., "WipeService")
     * @param message Log message
     */
    void log(LogLevel level, std::string_view component, std::string_view message);

    /**
     * @brief Log at DEBUG level
     * @param component Component name
     * @param message Log message
     */
    void debug(std::string_view component, std::string_view message);

    /**
     * @brief Log at INFO level
     * @param component Component name
     * @param message Log message
     */
    void info(std::string_view component, std::string_view message);

    /**
     * @brief Log at WARNING level
     * @param component Component name
     * @param message Log message
     */
    void warning(std::string_view component, std::string_view message);

    /**
     * @brief Log at ERROR level
     * @param component Component name
     * @param message Log message
     */
    void error(std::string_view component, std::string_view message);

    /**
     * @brief Flush pending log entries to disk
     */
    void flush();

    /**
     * @brief Set minimum log level
     * @param level New minimum level
     */
    void set_min_level(LogLevel level);

    /**
     * @brief Get current minimum log level
     * @return Current minimum level
     */
    [[nodiscard]] auto get_min_level() const -> LogLevel;

    /**
     * @brief Enable/disable console output (stderr)
     * @param enable Whether to also write to stderr
     */
    void set_console_output(bool enable);

    /**
     * @brief Get the current log file path
     * @return Path to the active log file, or empty if not initialized
     */
    [[nodiscard]] auto get_log_file_path() const -> std::filesystem::path;

    /**
     * @brief Shutdown the logger, flushing and closing files
     */
    void shutdown();

private:
    Logger() = default;
    ~Logger();

    /**
     * @brief Get ISO 8601 timestamp string
     * @return Formatted timestamp (e.g., "2026-01-22T14:32:45.123Z")
     */
    [[nodiscard]] static auto get_timestamp() -> std::string;

    /**
     * @brief Get string representation of log level
     * @param level Log level
     * @return Level string (e.g., "INFO ", "ERROR")
     */
    [[nodiscard]] static auto level_to_string(LogLevel level) -> std::string_view;

    /**
     * @brief Check if rotation is needed and perform it
     */
    void check_and_rotate();

    /**
     * @brief Perform log rotation
     */
    void rotate_logs();

    /**
     * @brief Open or reopen the log file
     * @return true if opened successfully
     */
    auto open_log_file() -> bool;

    // State
    mutable std::mutex mutex_;
    std::ofstream file_;
    std::filesystem::path log_dir_;
    std::string app_name_;
    LogLevel min_level_ = LogLevel::INFO;
    LogRotationPolicy policy_;
    bool initialized_ = false;
    bool console_output_ = false;
    size_t current_file_size_ = 0;
};

// Convenience macros for component-based logging
#define LOG_DEBUG(component, msg) ::util::Logger::instance().debug(component, msg)
#define LOG_INFO(component, msg) ::util::Logger::instance().info(component, msg)
#define LOG_WARNING(component, msg) ::util::Logger::instance().warning(component, msg)
#define LOG_ERROR(component, msg) ::util::Logger::instance().error(component, msg)

}  // namespace util
