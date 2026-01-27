#include "helper/services/WipeService.hpp"

#include "services/DevicePolicy.hpp"
#include "util/FileDescriptor.hpp"

// Algorithm implementations
#include "algorithms/ATASecureEraseAlgorithm.hpp"
#include "algorithms/DoD522022MAlgorithm.hpp"
#include "algorithms/GOSTAlgorithm.hpp"
#include "algorithms/GutmannAlgorithm.hpp"
#include "algorithms/RandomFillAlgorithm.hpp"
#include "algorithms/SchneierAlgorithm.hpp"
#include "algorithms/VSITRAlgorithm.hpp"
#include "algorithms/ZeroFillAlgorithm.hpp"

// Project headers
#include "util/Logger.hpp"

// Standard library
#include <cerrno>
#include <cstring>
#include <deque>
#include <format>
#include <utility>

// System headers
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Linux-specific headers
#include <linux/fs.h>

namespace {

/**
 * @brief Progress tracker that calculates speed and ETA
 *
 * Wraps a progress callback to add speed and ETA calculations
 * using a rolling average of recent write speeds.
 *
 * @note This class is NOT thread-safe. It is designed to be used
 *       exclusively from a single thread (the wipe worker thread).
 *       Do not share instances between threads or call methods
 *       concurrently.
 */
class ProgressTracker {
public:
    explicit ProgressTracker(ProgressCallback callback)
        : callback_(std::move(callback)), start_time_(std::chrono::steady_clock::now()),
          last_update_time_(start_time_), last_bytes_written_(0) {}

    void report(WipeProgress progress) {
        if (!callback_)
            return;

        auto now = std::chrono::steady_clock::now();

        // Calculate speed using time since last update
        auto elapsed_since_update =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time_).count();

        if (elapsed_since_update >= MIN_UPDATE_INTERVAL_MS &&
            progress.bytes_written > last_bytes_written_) {
            uint64_t bytes_delta = progress.bytes_written - last_bytes_written_;
            double seconds = static_cast<double>(elapsed_since_update) / 1000.0;

            if (seconds > 0) {
                auto current_speed =
                    static_cast<uint64_t>(static_cast<double>(bytes_delta) / seconds);

                // Add to rolling average
                speed_samples_.push_back(current_speed);
                if (speed_samples_.size() > MAX_SAMPLES) {
                    speed_samples_.pop_front();
                }

                // Calculate average speed
                uint64_t total_speed = 0;
                for (auto sample : speed_samples_) {
                    total_speed += sample;
                }
                progress.speed_bytes_per_sec = total_speed / speed_samples_.size();

                // Calculate ETA
                if (progress.speed_bytes_per_sec > 0 &&
                    progress.total_bytes > progress.bytes_written) {
                    uint64_t remaining_bytes = progress.total_bytes - progress.bytes_written;
                    // Account for remaining passes
                    if (progress.total_passes > progress.current_pass) {
                        remaining_bytes +=
                            progress.total_bytes *
                            static_cast<uint64_t>(progress.total_passes - progress.current_pass);
                    }
                    progress.estimated_seconds_remaining =
                        static_cast<int64_t>(remaining_bytes / progress.speed_bytes_per_sec);
                }

                last_update_time_ = now;
                last_bytes_written_ = progress.bytes_written;
            }
        } else if (!speed_samples_.empty()) {
            // Use last known speed between updates
            uint64_t total_speed = 0;
            for (auto sample : speed_samples_) {
                total_speed += sample;
            }
            progress.speed_bytes_per_sec = total_speed / speed_samples_.size();

            if (progress.speed_bytes_per_sec > 0 && progress.total_bytes > progress.bytes_written) {
                uint64_t remaining_bytes = progress.total_bytes - progress.bytes_written;
                if (progress.total_passes > progress.current_pass) {
                    remaining_bytes +=
                        progress.total_bytes *
                        static_cast<uint64_t>(progress.total_passes - progress.current_pass);
                }
                progress.estimated_seconds_remaining =
                    static_cast<int64_t>(remaining_bytes / progress.speed_bytes_per_sec);
            }
        }

        callback_(progress);
    }

private:
    static constexpr size_t MAX_SAMPLES = 10;               // Rolling average window
    static constexpr int64_t MIN_UPDATE_INTERVAL_MS = 100;  // Minimum ms between speed calculations

    ProgressCallback callback_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_time_;
    uint64_t last_bytes_written_;
    std::deque<uint64_t> speed_samples_;
};

}  // namespace

WipeService::WipeService(std::shared_ptr<IDiskService> disk_service)
    : disk_service_(std::move(disk_service)) {
    state_ = std::make_shared<ThreadState>();
    initialize_algorithms();
}

WipeService::~WipeService() {
    if (state_->operation_in_progress.load()) {
        state_->cancel_requested.store(true);

        // Wait for thread with timeout - log warning but continue waiting
        auto start = std::chrono::steady_clock::now();
        while (state_->operation_in_progress.load()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= SHUTDOWN_TIMEOUT) {
                // Log critical warning but still wait for join
                // Better to block shutdown than corrupt data by detaching
                LOG_ERROR(
                    "WipeService",
                    std::format("Shutdown - thread did not respond to cancel within "
                                "{}s timeout. Waiting for thread to complete to prevent "
                                "data corruption.",
                                std::chrono::duration_cast<std::chrono::seconds>(SHUTDOWN_TIMEOUT)
                                    .count()));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    std::lock_guard lock(thread_mutex_);
    if (wipe_thread_.joinable()) {
        // Always join - never detach. If thread is stuck, we wait.
        // Detaching a wipe thread can lead to data corruption if the process
        // exits while writing to disk.
        wipe_thread_.join();
    }
}

void WipeService::initialize_algorithms() {
    algorithms_[WipeAlgorithm::ZERO_FILL] = std::make_shared<ZeroFillAlgorithm>();
    algorithms_[WipeAlgorithm::RANDOM_FILL] = std::make_shared<RandomFillAlgorithm>();
    algorithms_[WipeAlgorithm::DOD_5220_22_M] = std::make_shared<DoD522022MAlgorithm>();
    algorithms_[WipeAlgorithm::SCHNEIER] = std::make_shared<SchneierAlgorithm>();
    algorithms_[WipeAlgorithm::VSITR] = std::make_shared<VSITRAlgorithm>();
    algorithms_[WipeAlgorithm::GUTMANN] = std::make_shared<GutmannAlgorithm>();
    algorithms_[WipeAlgorithm::GOST_R_50739_95] = std::make_shared<GOSTAlgorithm>();
    algorithms_[WipeAlgorithm::ATA_SECURE_ERASE] = std::make_shared<ATASecureEraseAlgorithm>();
}

auto WipeService::get_algorithm(WipeAlgorithm algo) const -> std::shared_ptr<IWipeAlgorithm> {
    auto it = algorithms_.find(algo);
    if (it != algorithms_.end()) {
        return it->second;
    }
    return nullptr;
}

auto WipeService::prepare_wipe(const std::string& disk_path, WipeAlgorithm algorithm,
                               const ProgressCallback& callback) -> std::optional<WipePreparation> {
    if (state_->operation_in_progress.load()) {
        return std::nullopt;  // Operation already in progress
    }

    if (!disk_service_) {
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = "Disk service not configured";
            progress.is_complete = true;
            callback(progress);
        }
        return std::nullopt;
    }

    if (auto eligible = device_policy::validate_wipe_target(*disk_service_, disk_path); !eligible) {
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = eligible.error().message;
            progress.is_complete = true;
            callback(progress);
        }
        return std::nullopt;
    }

    // Join previous thread if it exists (with lock)
    {
        std::lock_guard lock(thread_mutex_);
        if (wipe_thread_.joinable()) {
            wipe_thread_.join();
        }
    }

    state_->cancel_requested.store(false);
    state_->operation_in_progress.store(true);

    auto algorithm_ptr = get_algorithm(algorithm);
    if (!algorithm_ptr) {
        state_->operation_in_progress.store(false);
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = "Unknown algorithm";
            callback(progress);
        }
        return std::nullopt;
    }

    return WipePreparation{.algorithm = algorithm_ptr,
                           .requires_device_access = algorithm_ptr->requires_device_access()};
}

auto WipeService::execute_wipe_on_device(
    const std::string& disk_path, const std::shared_ptr<IWipeAlgorithm>& algorithm_ptr,
    bool requires_device_access, const std::function<void(const WipeProgress&)>& tracked_callback,
    std::shared_ptr<ThreadState> state) -> WipeResult {
    uint64_t device_size = 0;
    bool result = false;

    // Some algorithms (like ATA Secure Erase) need device-level access
    if (requires_device_access) {
        // Get device size first
        util::FileDescriptor probe_fd(open(disk_path.c_str(), O_RDONLY));
        if (probe_fd) {
            ioctl(probe_fd.get(), BLKGETSIZE64, &device_size);
        }
        // probe_fd closed automatically

        // Use execute_on_device which handles the device internally
        result = algorithm_ptr->execute_on_device(disk_path, device_size, tracked_callback,
                                                  state->cancel_requested);
    } else {
        util::FileDescriptor fd(open(disk_path.c_str(), O_WRONLY | O_SYNC));
        if (!fd) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = "Failed to open device: " + std::string(strerror(errno));
            progress.is_complete = true;
            tracked_callback(progress);
            state->operation_in_progress.store(false);
            return {.success = false, .device_size = 0};
        }

        if (ioctl(fd.get(), BLKGETSIZE64, &device_size) == -1) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = "Failed to get device size";
            progress.is_complete = true;
            tracked_callback(progress);
            state->operation_in_progress.store(false);
            return {.success = false, .device_size = 0};
        }

        result = algorithm_ptr->execute(fd.get(), device_size, tracked_callback,
                                        state->cancel_requested);

        if (fsync(fd.get()) != 0) {
            // Log sync error but don't fail the operation
            LOG_WARNING("WipeService", std::format("fsync failed: {}", strerror(errno)));
        }
        // fd automatically closed by RAII
    }

    return {.success = result, .device_size = device_size};
}

auto WipeService::build_completion_status(bool wipe_result, bool do_verify, bool verify_result,
                                          bool cancelled) -> WipeProgress {
    WipeProgress final_progress{};
    final_progress.is_complete = true;
    final_progress.has_error = !wipe_result || (do_verify && !verify_result);
    final_progress.percentage = wipe_result ? 100.0 : 0.0;
    final_progress.verification_enabled = do_verify;
    final_progress.verification_passed = verify_result;

    if (cancelled) {
        final_progress.status = "Operation cancelled";
        final_progress.has_error = true;
        final_progress.error_message = "Operation was cancelled by user";
    } else if (wipe_result && do_verify && !verify_result) {
        final_progress.status = "Wipe completed but verification failed";
        final_progress.error_message = "Verification failed: data does not match expected pattern";
    } else if (wipe_result) {
        if (do_verify) {
            final_progress.status = "Wipe and verification completed successfully";
        } else {
            final_progress.status = "Wipe completed successfully";
        }
    } else if (!final_progress.has_error) {
        final_progress.has_error = true;
        final_progress.error_message = "Wipe operation failed";
    }

    return final_progress;
}

auto WipeService::wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                            ProgressCallback callback) -> bool {
    // Delegate to the full overload with verify=false
    return wipe_disk(disk_path, algorithm, std::move(callback), false);
}

auto WipeService::cancel_current_operation() -> bool {
    if (state_->operation_in_progress.load()) {
        state_->cancel_requested.store(true);
        // Thread will check cancel_requested flag and terminate gracefully
        // No need to join here - let the operation finish on its own
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

auto WipeService::supports_verification(WipeAlgorithm algo) -> bool {
    auto algorithm = get_algorithm(algo);
    if (algorithm) {
        return algorithm->supports_verification();
    }
    return false;
}

auto WipeService::wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                            ProgressCallback callback, bool verify) -> bool {
    // Validate and prepare for wipe
    auto preparation = prepare_wipe(disk_path, algorithm, callback);
    if (!preparation) {
        return false;
    }

    // Check if verification is requested but not supported
    const bool do_verify = verify && preparation->algorithm->supports_verification();

    // Run wipe operation in separate thread
    std::lock_guard lock(thread_mutex_);
    wipe_thread_ =
        std::thread([disk_path, callback, state = state_, algorithm_ptr = preparation->algorithm,
                     requires_device_access = preparation->requires_device_access, do_verify]() {
            bool wipe_result = false;
            bool verify_result = true;
            uint64_t device_size = 0;

            // Create progress tracker to calculate speed and ETA
            auto tracker = std::make_shared<ProgressTracker>(callback);
            auto tracked_callback = [tracker, do_verify](const WipeProgress& progress) {
                WipeProgress p = progress;
                p.verification_enabled = do_verify;
                tracker->report(p);
            };

            try {
                // Execute the wipe operation
                auto result = execute_wipe_on_device(
                    disk_path, algorithm_ptr, requires_device_access, tracked_callback, state);

                // Check for early exit (device open/size failure already reported)
                if (!result.success && result.device_size == 0 &&
                    !state->operation_in_progress.load()) {
                    return;  // Error already handled and reported
                }

                wipe_result = result.success;
                device_size = result.device_size;

                // Perform verification if requested, wipe succeeded, and not cancelled
                if (do_verify && wipe_result && !state->cancel_requested.load()) {
                    // Reopen device for reading
                    util::FileDescriptor verify_fd(open(disk_path.c_str(), O_RDONLY));
                    if (!verify_fd) {
                        WipeProgress progress{};
                        progress.has_error = true;
                        progress.error_message = "Failed to open device for verification";
                        progress.is_complete = true;
                        tracked_callback(progress);
                        state->operation_in_progress.store(false);
                        return;
                    }

                    // Create verification progress callback
                    auto verify_callback = [&tracked_callback](const WipeProgress& progress) {
                        WipeProgress p = progress;
                        p.verification_in_progress = true;
                        p.status = "Verifying wipe...";
                        tracked_callback(p);
                    };

                    verify_result = algorithm_ptr->verify(verify_fd.get(), device_size,
                                                          verify_callback, state->cancel_requested);
                }

            } catch (const std::exception& e) {
                WipeProgress progress{};
                progress.has_error = true;
                progress.error_message = "Wipe operation failed: " + std::string(e.what());
                tracked_callback(progress);
                wipe_result = false;
            }

            // Build and send completion status
            auto final_progress = build_completion_status(wipe_result, do_verify, verify_result,
                                                          state->cancel_requested.load());
            tracked_callback(final_progress);

            state->operation_in_progress.store(false);
        });

    return true;
}
