#include "services/WipeService.hpp"
#include "algorithms/AlgorithmRegistry.hpp"

#include <glib.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <sstream>
#include <utility>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

namespace {

// Helper binary locations (in order of preference)
constexpr const char* HELPER_PATHS[] = {
    "/usr/lib/storage-wiper/storage-wiper-helper",
    "/usr/local/lib/storage-wiper/storage-wiper-helper",
    "/usr/libexec/storage-wiper-helper",
    "/usr/bin/storage-wiper-helper"
};

auto find_helper_binary() -> std::string {
    for (const auto* path : HELPER_PATHS) {
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) {
            return path;
        }
    }
    return "";
}

auto algorithm_to_string(WipeAlgorithm algo) -> std::string {
    switch (algo) {
        case WipeAlgorithm::ZERO_FILL: return "zero";
        case WipeAlgorithm::RANDOM_FILL: return "random";
        case WipeAlgorithm::DOD_5220_22_M: return "dod-5220-22-m";
        case WipeAlgorithm::SCHNEIER: return "schneier";
        case WipeAlgorithm::VSITR: return "vsitr";
        case WipeAlgorithm::GOST_R_50739_95: return "gost";
        case WipeAlgorithm::GUTMANN: return "gutmann";
        default: return "zero";
    }
}

// Parse progress line: PROGRESS:current:total:percentage
auto parse_progress_line(const std::string& line, WipeProgress& progress) -> bool {
    if (!line.starts_with("PROGRESS:")) {
        return false;
    }

    std::istringstream ss(line.substr(9)); // Skip "PROGRESS:"
    char delim;
    int current_pass, total_passes;
    double percentage;

    if (ss >> current_pass >> delim >> total_passes >> delim >> percentage) {
        progress.current_pass = current_pass;
        progress.total_passes = total_passes;
        progress.percentage = percentage;
        progress.status = std::format("Pass {} of {} - {:.1f}%", current_pass, total_passes, percentage);
        return true;
    }
    return false;
}

// Parse completion line: COMPLETE:status[:message]
auto parse_complete_line(const std::string& line, WipeProgress& progress) -> bool {
    if (!line.starts_with("COMPLETE:")) {
        return false;
    }

    progress.is_complete = true;
    std::string rest = line.substr(9); // Skip "COMPLETE:"

    if (rest.starts_with("success")) {
        progress.has_error = false;
        progress.percentage = 100.0;
        progress.status = "Wipe completed successfully";
    } else if (rest.starts_with("cancelled")) {
        progress.has_error = true;
        progress.error_message = "Operation was cancelled by user";
        progress.status = "Operation cancelled";
    } else if (rest.starts_with("error:")) {
        progress.has_error = true;
        progress.error_message = rest.substr(6);
        progress.status = "Wipe failed";
    } else {
        progress.has_error = true;
        progress.error_message = "Unknown completion status";
    }

    return true;
}

} // anonymous namespace

WipeService::WipeService() {
    state_ = std::make_shared<ThreadState>();
    initialize_algorithms();
}

WipeService::~WipeService() {
    // Cancel any running operation
    if (state_->operation_in_progress.load()) {
        cancel_current_operation();

        // Wait for thread with timeout
        auto start = std::chrono::steady_clock::now();
        while (state_->operation_in_progress.load()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= SHUTDOWN_TIMEOUT) {
                std::cerr << "Warning: Wipe operation did not terminate within timeout" << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    std::lock_guard lock(thread_mutex_);
    if (wipe_thread_.joinable()) {
        if (state_->operation_in_progress.load()) {
            wipe_thread_.detach();
        } else {
            wipe_thread_.join();
        }
    }
}

void WipeService::initialize_algorithms() {
    // Use the auto-registration system to get all registered algorithms
    // (This is kept for algorithm info queries)
    algorithms_ = algorithms::AlgorithmRegistry::instance().create_all();
}

auto WipeService::get_algorithm(WipeAlgorithm algo) const -> std::shared_ptr<IWipeAlgorithm> {
    auto it = algorithms_.find(algo);
    if (it != algorithms_.end()) {
        return it->second;
    }
    return nullptr;
}

auto WipeService::wipe_disk(const std::string& disk_path,
                            WipeAlgorithm algorithm,
                            ProgressCallback callback,
                            bool unmount_first) -> bool {

    if (state_->operation_in_progress.load()) {
        return false; // Operation already in progress
    }

    // ATA Secure Erase is not supported via helper (needs special handling)
    if (algorithm == WipeAlgorithm::ATA_SECURE_ERASE) {
        if (callback) {
            WipeProgress progress{};
            progress.is_complete = true;
            progress.has_error = true;
            progress.error_message = "ATA Secure Erase is not yet supported in this version";
            callback(progress);
        }
        return false;
    }

    // Find helper binary
    std::string helper_path = find_helper_binary();
    if (helper_path.empty()) {
        if (callback) {
            WipeProgress progress{};
            progress.is_complete = true;
            progress.has_error = true;
            progress.error_message = "Helper binary not found. Please reinstall the application.";
            callback(progress);
        }
        return false;
    }

    // Join previous thread if exists
    {
        std::lock_guard lock(thread_mutex_);
        if (wipe_thread_.joinable()) {
            wipe_thread_.join();
        }
    }

    state_->cancel_requested.store(false);
    state_->operation_in_progress.store(true);
    state_->child_pid.store(0);

    std::string algo_str = algorithm_to_string(algorithm);

    // Run wipe operation in separate thread
    std::lock_guard lock(thread_mutex_);
    wipe_thread_ = std::thread([disk_path, algo_str, helper_path, callback, state = state_, unmount_first]() {
        // Build command: pkexec helper --device /dev/xxx --algorithm yyy [--unmount]
        std::vector<gchar*> argv_vec = {
            const_cast<gchar*>("pkexec"),
            const_cast<gchar*>(helper_path.c_str()),
            const_cast<gchar*>("--device"),
            const_cast<gchar*>(disk_path.c_str()),
            const_cast<gchar*>("--algorithm"),
            const_cast<gchar*>(algo_str.c_str())
        };

        if (unmount_first) {
            argv_vec.push_back(const_cast<gchar*>("--unmount"));
        }
        argv_vec.push_back(nullptr);

        gchar** argv = argv_vec.data();

        gint stdout_fd = -1;
        gint stderr_fd = -1;
        GPid child_pid = 0;
        GError* error = nullptr;

        gboolean spawned = g_spawn_async_with_pipes(
            nullptr,           // working directory
            argv,              // arguments
            nullptr,           // environment (inherit)
            static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
            nullptr,           // child setup
            nullptr,           // user data
            &child_pid,        // child PID
            nullptr,           // stdin
            &stdout_fd,        // stdout
            &stderr_fd,        // stderr
            &error
        );

        if (!spawned) {
            if (callback) {
                WipeProgress progress{};
                progress.is_complete = true;
                progress.has_error = true;
                progress.error_message = std::format("Failed to spawn helper: {}",
                    error ? error->message : "Unknown error");
                callback(progress);
            }
            if (error) g_error_free(error);
            state->operation_in_progress.store(false);
            return;
        }

        state->child_pid.store(child_pid);

        // Read stdout for progress updates
        GIOChannel* stdout_channel = g_io_channel_unix_new(stdout_fd);
        g_io_channel_set_flags(stdout_channel, G_IO_FLAG_NONBLOCK, nullptr);

        WipeProgress final_progress{};
        std::string line_buffer;

        while (!state->cancel_requested.load()) {
            gchar* line = nullptr;
            gsize length = 0;
            GIOStatus status = g_io_channel_read_line(stdout_channel, &line, &length, nullptr, nullptr);

            if (status == G_IO_STATUS_NORMAL && line) {
                std::string line_str(line);
                g_free(line);

                // Remove trailing newline
                while (!line_str.empty() && (line_str.back() == '\n' || line_str.back() == '\r')) {
                    line_str.pop_back();
                }

                WipeProgress progress{};
                if (parse_progress_line(line_str, progress)) {
                    if (callback) {
                        callback(progress);
                    }
                } else if (parse_complete_line(line_str, progress)) {
                    final_progress = progress;
                    break;
                }
            } else if (status == G_IO_STATUS_AGAIN) {
                // No data available, wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds{50});

                // Check if child is still running
                int wait_status = 0;
                pid_t result = waitpid(child_pid, &wait_status, WNOHANG);
                if (result == child_pid) {
                    // Child has exited
                    if (!final_progress.is_complete) {
                        final_progress.is_complete = true;
                        if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
                            final_progress.has_error = false;
                            final_progress.percentage = 100.0;
                            final_progress.status = "Wipe completed successfully";
                        } else {
                            final_progress.has_error = true;
                            final_progress.error_message = "Helper process exited unexpectedly";
                        }
                    }
                    break;
                }
            } else if (status == G_IO_STATUS_EOF) {
                break;
            } else {
                // Error
                break;
            }
        }

        // Handle cancellation
        if (state->cancel_requested.load() && state->child_pid.load() > 0) {
            kill(state->child_pid.load(), SIGTERM);
            final_progress.is_complete = true;
            final_progress.has_error = true;
            final_progress.error_message = "Operation was cancelled by user";
            final_progress.status = "Operation cancelled";
        }

        // Cleanup
        g_io_channel_unref(stdout_channel);
        close(stdout_fd);
        close(stderr_fd);
        g_spawn_close_pid(child_pid);

        // Wait for child to fully exit
        int wait_status = 0;
        waitpid(child_pid, &wait_status, 0);

        // Send final callback
        if (callback && final_progress.is_complete) {
            callback(final_progress);
        } else if (callback) {
            // Ensure we always send completion
            final_progress.is_complete = true;
            if (!final_progress.has_error) {
                final_progress.has_error = true;
                final_progress.error_message = "Operation ended unexpectedly";
            }
            callback(final_progress);
        }

        state->child_pid.store(0);
        state->operation_in_progress.store(false);
    });

    return true;
}

auto WipeService::cancel_current_operation() -> bool {
    if (state_->operation_in_progress.load()) {
        state_->cancel_requested.store(true);

        // Send SIGTERM to child process if running
        GPid pid = state_->child_pid.load();
        if (pid > 0) {
            kill(pid, SIGTERM);
        }

        return true;
    }
    return false;
}

auto WipeService::get_algorithm_name(WipeAlgorithm algo) -> std::string {
    auto algorithm = get_algorithm(algo);
    if (algorithm) {
        return algorithm->get_name();
    }
    return "Unknown";
}

auto WipeService::get_algorithm_description(WipeAlgorithm algo) -> std::string {
    auto algorithm = get_algorithm(algo);
    if (algorithm) {
        return algorithm->get_description();
    }
    return "Unknown algorithm";
}

auto WipeService::get_pass_count(WipeAlgorithm algo) -> int {
    auto algorithm = get_algorithm(algo);
    if (algorithm) {
        return algorithm->get_pass_count();
    }
    return 0;
}

auto WipeService::is_ssd_compatible(WipeAlgorithm algo) -> bool {
    auto algorithm = get_algorithm(algo);
    if (algorithm) {
        return algorithm->is_ssd_compatible();
    }
    return false;
}
