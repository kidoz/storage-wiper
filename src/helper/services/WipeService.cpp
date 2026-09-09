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
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <utility>

// System headers
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Linux-specific headers
#include <linux/fs.h>

namespace fs = std::filesystem;

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
        : callback_(std::move(callback)), last_update_time_(std::chrono::steady_clock::now()),
          last_bytes_written_(0) {}

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
    std::chrono::steady_clock::time_point last_update_time_;
    uint64_t last_bytes_written_;
    std::deque<uint64_t> speed_samples_;
};

/**
 * @brief Resolve the kernel name of the disk that owns a device
 *
 * For whole disks this is the device's own name; for partitions it is found
 * via the sysfs layout (/sys/block/<disk>/<partition>).
 */
auto parent_disk_name(const std::string& device_path) -> std::string {
    const auto name = fs::path{device_path}.filename().string();

    std::error_code ec;
    if (fs::exists(std::format("/sys/block/{}/queue/rotational", name), ec)) {
        return name;
    }

    std::error_code iter_ec;
    for (fs::directory_iterator it{"/sys/block", iter_ec}, end; it != end && !iter_ec;
         it.increment(iter_ec)) {
        std::error_code ec2;
        if (fs::exists(it->path() / name / "partition", ec2) && !ec2) {
            return it->path().filename().string();
        }
    }
    return name;
}

/**
 * @brief Check whether the device is an SSD (non-rotational media)
 */
auto is_non_rotational_device(const std::string& device_path) -> bool {
    std::ifstream file{
        std::format("/sys/block/{}/queue/rotational", parent_disk_name(device_path))};
    if (!file) {
        return false;  // unknown: do not touch the device with discards
    }
    int rotational = 1;
    file >> rotational;
    return rotational == 0;
}

/**
 * @brief Issue BLKDISCARD over the whole device to restore SSD performance
 *
 * Runs after a successful wipe (and its verification, if any), so the
 * discard can never hide wipe results from the read-back. Failures are
 * non-fatal: devices and bridges that do not support discard are simply
 * skipped.
 *
 * @return true when at least the first discard was accepted
 */
auto issue_trim_if_ssd(int fd, const std::string& disk_path, uint64_t device_size) -> bool {
    if (device_size == 0 || !is_non_rotational_device(disk_path)) {
        return false;
    }

    constexpr uint64_t DISCARD_CHUNK = uint64_t{1} << 30;  // 1 GiB per request
    uint64_t offset = 0;
    bool supported = true;

    while (offset < device_size) {
        const uint64_t range[2] = {offset, std::min(DISCARD_CHUNK, device_size - offset)};
        if (ioctl(fd, BLKDISCARD, range) != 0) {
            supported = false;
            break;
        }
        offset += range[1];
    }

    if (supported) {
        LOG_INFO("WipeService",
                 std::format("BLKDISCARD issued for {} ({} bytes)", disk_path, device_size));
    }
    return supported;
}

}  // namespace

WipeService::WipeService(std::shared_ptr<IDiskService> disk_service)
    : disk_service_(std::move(disk_service)) {
    initialize_algorithms();
}

WipeService::~WipeService() {
    // Snapshot the operations so workers, which never touch the map, are safe
    std::map<std::string, std::shared_ptr<Operation>> ops;
    {
        std::lock_guard lock(operations_mutex_);
        ops = operations_;
    }

    // Ask every live operation to stop
    for (auto& [path, op] : ops) {
        if (op->state->operation_in_progress.load()) {
            op->state->cancel_requested.store(true);
        }
    }

    // Wait (bounded) for the cancellations to take effect
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        bool all_stopped = true;
        for (auto& [path, op] : ops) {
            if (op->state->operation_in_progress.load()) {
                all_stopped = false;
                break;
            }
        }
        if (all_stopped) {
            break;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= SHUTDOWN_TIMEOUT) {
            // Log critical warning but still wait for join
            // Better to block shutdown than corrupt data by detaching
            LOG_ERROR(
                "WipeService",
                std::format(
                    "Shutdown - some operations did not respond to cancel within "
                    "{}s timeout. Waiting for their threads to complete to prevent "
                    "data corruption.",
                    std::chrono::duration_cast<std::chrono::seconds>(SHUTDOWN_TIMEOUT).count()));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    for (auto& [path, op] : ops) {
        if (op->thread.joinable()) {
            // Always join - never detach. If a thread is stuck, we wait.
            // Detaching a wipe thread can lead to data corruption if the
            // process exits while writing to disk.
            op->thread.join();
        }
    }

    std::lock_guard lock(operations_mutex_);
    operations_.clear();
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

auto WipeService::is_operation_in_progress(const std::string& disk_path) -> bool {
    std::lock_guard lock(operations_mutex_);
    const auto it = operations_.find(disk_path);
    return it != operations_.end() && it->second->state->operation_in_progress.load();
}

auto WipeService::cancel_operation(const std::string& disk_path) -> bool {
    std::lock_guard lock(operations_mutex_);
    const auto it = operations_.find(disk_path);
    if (it == operations_.end() || !it->second->state->operation_in_progress.load()) {
        return false;
    }
    // The worker checks the flag and terminates gracefully; the entry is
    // reaped when the same device is wiped again or on destruction.
    it->second->state->cancel_requested.store(true);
    return true;
}

auto WipeService::prepare_wipe(const std::string& disk_path, WipeAlgorithm algorithm,
                               const ProgressCallback& callback) -> std::optional<WipePreparation> {
    // Claim the per-device operation slot. Wipes on different devices run in
    // parallel; a second wipe on the same device is rejected until the first
    // has finished.
    {
        std::lock_guard lock(operations_mutex_);
        if (const auto it = operations_.find(disk_path); it != operations_.end()) {
            if (it->second->state->operation_in_progress.load()) {
                return std::nullopt;  // Operation already in progress on this device
            }
        }
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

    auto targets = device_policy::resolve_wipe_targets(*disk_service_, disk_path, algorithm);
    if (!targets) {
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = targets.error().message;
            progress.is_complete = true;
            callback(progress);
        }
        return std::nullopt;
    }

    auto algorithm_ptr = get_algorithm(algorithm);
    if (!algorithm_ptr) {
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = "Unknown algorithm";
            callback(progress);
        }
        return std::nullopt;
    }

    // Reap the previous finished operation for this device (join + erase) and
    // insert the new one atomically. Joining here is deadlock-free: workers
    // never take operations_mutex_.
    auto operation = std::make_shared<Operation>();
    operation->targets = std::move(*targets);
    {
        std::lock_guard lock(operations_mutex_);
        // Reserve the entire affected scope atomically, including overlaps
        // between a disk and its partitions or controller-wide NVMe erases.
        for (const auto& [path, active] : operations_) {
            if (!active->state->operation_in_progress.load()) {
                continue;
            }
            for (const auto& target : operation->targets) {
                for (const auto& claimed : active->targets) {
                    if (device_path_matcher::is_device_or_partition_of(target, claimed) ||
                        device_path_matcher::is_device_or_partition_of(claimed, target)) {
                        return std::nullopt;
                    }
                }
            }
        }
        if (const auto it = operations_.find(disk_path); it != operations_.end()) {
            if (it->second->thread.joinable()) {
                it->second->thread.join();
            }
            operations_.erase(it);
        }
        operation->state->operation_in_progress.store(true);
        operations_.emplace(disk_path, operation);
    }

    return WipePreparation{.algorithm = algorithm_ptr,
                           .requires_device_access = algorithm_ptr->requires_device_access(),
                           .operation = std::move(operation)};
}

auto WipeService::build_completion_status(bool wipe_result, bool do_verify, bool verify_result,
                                          bool cancelled, uint64_t bad_blocks, bool trim_issued)
    -> WipeProgress {
    WipeProgress final_progress{};
    final_progress.is_complete = true;
    final_progress.has_error = !wipe_result || (do_verify && !verify_result);
    final_progress.percentage = wipe_result ? 100.0 : 0.0;
    final_progress.verification_enabled = do_verify;
    final_progress.verification_passed = verify_result;
    final_progress.bad_block_count = bad_blocks;

    auto success_note = [trim_issued]() -> std::string {
        return trim_issued ? " TRIM/discard issued." : "";
    };
    auto bad_block_note = [bad_blocks]() -> std::string {
        if (bad_blocks == 0) {
            return "";
        }
        return std::format(" {} bad sectors could not be overwritten; data in them may "
                           "survive an overwrite wipe.",
                           bad_blocks);
    };

    if (cancelled) {
        final_progress.status = "Operation cancelled";
        final_progress.has_error = true;
        final_progress.error_message = "Operation was cancelled by user";
    } else if (wipe_result && do_verify && !verify_result) {
        final_progress.status = "Wipe completed but verification failed";
        final_progress.error_message = "Verification failed: data does not match expected pattern";
    } else if (wipe_result) {
        if (do_verify) {
            final_progress.status =
                "Wipe and verification completed successfully." + success_note() + bad_block_note();
        } else {
            final_progress.status =
                "Wipe completed successfully." + success_note() + bad_block_note();
        }
    } else {
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
    if (verify && !do_verify) {
        // Never silently ignore an explicit --verify request: log it so the
        // audit trail shows verification did not run.
        LOG_WARNING("WipeService",
                    std::format("Verification requested but not supported by algorithm {}; "
                                "verification will be skipped",
                                preparation->algorithm->get_name()));
    }

    // Only the service emits a terminal event, after writes, verification and
    // discard have finished. Algorithm terminal events remain progress updates.
    auto operation = preparation->operation;
    operation->thread = std::thread([disk_path, callback = std::move(callback), operation,
                                     algorithm_ptr = preparation->algorithm,
                                     requires_device_access = preparation->requires_device_access,
                                     do_verify]() {
        const auto& state = operation->state;
        bool wipe_result = false;
        bool verify_result = false;
        bool trim_issued = false;
        uint64_t device_size = 0;
        WipeProgress last_write{};
        std::string error_message;
        ProgressTracker tracker(callback);
        auto tracked_callback = [&](const WipeProgress& progress) {
            WipeProgress p = progress;
            if (!p.error_message.empty()) {
                error_message = p.error_message;
            }
            if (!p.verification_in_progress && !p.has_error) {
                last_write = p;
            }
            p.is_complete = false;
            p.verification_enabled = do_verify;
            p.bad_block_count = last_write.bad_block_count;
            tracker.report(p);
        };

        // Keep every exclusive claim alive until the operation finishes.
        // Firmware commands reopen devices internally, but these retained
        // claims prevent mounts throughout that interval as well.
        std::vector<util::FileDescriptor> claims;
        try {
            for (const auto& target : operation->targets) {
                claims.emplace_back(open(target.c_str(), O_RDWR | O_SYNC | O_EXCL));
                if (!claims.back()) {
                    throw std::runtime_error("Cannot exclusively claim " + target + ": " +
                                             std::string(strerror(errno)));
                }
            }
            const int fd = claims.front().get();
            if (ioctl(fd, BLKGETSIZE64, &device_size) == -1 || device_size == 0) {
                throw std::runtime_error("Failed to get a nonzero device size");
            }
            if (!state->cancel_requested.load()) {
                wipe_result =
                    requires_device_access
                        ? algorithm_ptr->execute_on_device(disk_path, device_size, tracked_callback,
                                                           state->cancel_requested)
                        : algorithm_ptr->execute(fd, device_size, tracked_callback,
                                                 state->cancel_requested);
                if (!requires_device_access && fsync(fd) != 0) {
                    throw std::runtime_error("Failed to flush data to disk: " +
                                             std::string(strerror(errno)));
                }
            }

            if (do_verify && wipe_result && !state->cancel_requested.load()) {
                auto verify_callback = [&](const WipeProgress& progress) {
                    WipeProgress p = progress;
                    p.verification_in_progress = true;
                    p.status = "Verifying wipe...";
                    tracked_callback(p);
                };
                verify_result = algorithm_ptr->verify(fd, device_size, verify_callback,
                                                      state->cancel_requested);
            }
            if (wipe_result && (!do_verify || verify_result) && !state->cancel_requested.load()) {
                trim_issued = issue_trim_if_ssd(fd, disk_path, device_size);
            }
        } catch (const std::exception& error) {
            error_message = error.what();
            wipe_result = false;
        }

        auto final_progress = build_completion_status(wipe_result, do_verify, verify_result,
                                                      state->cancel_requested.load(),
                                                      last_write.bad_block_count, trim_issued);
        final_progress.total_bytes = device_size;
        final_progress.bytes_written = last_write.bytes_written;
        final_progress.total_passes = algorithm_ptr->get_pass_count();
        final_progress.current_pass = last_write.current_pass;
        if (final_progress.has_error && !error_message.empty()) {
            final_progress.error_message = error_message;
        }
        tracker.report(final_progress);
        state->operation_in_progress.store(false);
    });
    return true;
}
