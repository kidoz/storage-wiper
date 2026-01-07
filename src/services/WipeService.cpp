#include "services/WipeService.hpp"
#include "services/DevicePolicy.hpp"
#include "util/FileDescriptor.hpp"

// Algorithm implementations
#include "algorithms/ZeroFillAlgorithm.hpp"
#include "algorithms/RandomFillAlgorithm.hpp"
#include "algorithms/DoD522022MAlgorithm.hpp"
#include "algorithms/SchneierAlgorithm.hpp"
#include "algorithms/VSITRAlgorithm.hpp"
#include "algorithms/GutmannAlgorithm.hpp"
#include "algorithms/GOSTAlgorithm.hpp"
#include "algorithms/ATASecureEraseAlgorithm.hpp"

// Standard library
#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>

// System headers
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Linux-specific headers
#include <linux/fs.h>

WipeService::WipeService(std::shared_ptr<IDiskService> disk_service)
    : disk_service_(std::move(disk_service)) {
    state_ = std::make_shared<ThreadState>();
    initialize_algorithms();
}

WipeService::~WipeService() {
    if (state_->operation_in_progress.load()) {
        state_->cancel_requested.store(true);

        // Wait for thread with timeout to avoid blocking indefinitely
        auto start = std::chrono::steady_clock::now();
        while (state_->operation_in_progress.load()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= SHUTDOWN_TIMEOUT) {
                std::cerr << "Warning: Wipe thread did not terminate within timeout" << std::endl;
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

auto WipeService::wipe_disk(const std::string& disk_path,
                            WipeAlgorithm algorithm,
                            ProgressCallback callback) -> bool {

    if (state_->operation_in_progress.load()) {
        return false; // Operation already in progress
    }

    if (!disk_service_) {
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = "Disk service not configured";
            progress.is_complete = true;
            callback(progress);
        }
        return false;
    }

    if (auto eligible = device_policy::validate_wipe_target(*disk_service_, disk_path); !eligible) {
        if (callback) {
            WipeProgress progress{};
            progress.has_error = true;
            progress.error_message = eligible.error().message;
            progress.is_complete = true;
            callback(progress);
        }
        return false;
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
        return false;
    }

    // Run wipe operation in separate thread
    const bool requires_device_access = algorithm_ptr->requires_device_access();

    std::lock_guard lock(thread_mutex_);
    wipe_thread_ = std::thread([disk_path, callback, state = state_, algorithm_ptr, requires_device_access]() {
        bool result = false;

        try {
            // Some algorithms (like ATA Secure Erase) need device-level access
            if (requires_device_access) {
                // Get device size first
                util::FileDescriptor probe_fd(open(disk_path.c_str(), O_RDONLY));
                uint64_t size = 0;
                if (probe_fd) {
                    ioctl(probe_fd.get(), BLKGETSIZE64, &size);
                }
                // probe_fd closed automatically

                // Use execute_on_device which handles the device internally
                result = algorithm_ptr->execute_on_device(disk_path, size, callback, state->cancel_requested);
            } else {
                util::FileDescriptor fd(open(disk_path.c_str(), O_WRONLY | O_SYNC));
                if (!fd) {
                    if (callback) {
                        WipeProgress progress{};
                        progress.has_error = true;
                        progress.error_message = "Failed to open device: " + std::string(strerror(errno));
                        progress.is_complete = true;
                        callback(progress);
                    }
                    state->operation_in_progress.store(false);
                    return;
                }

                uint64_t size;
                if (ioctl(fd.get(), BLKGETSIZE64, &size) == -1) {
                    if (callback) {
                        WipeProgress progress{};
                        progress.has_error = true;
                        progress.error_message = "Failed to get device size";
                        progress.is_complete = true;
                        callback(progress);
                    }
                    state->operation_in_progress.store(false);
                    return;
                }

                result = algorithm_ptr->execute(fd.get(), size, callback, state->cancel_requested);

                if (fsync(fd.get()) != 0) {
                    // Log sync error but don't fail the operation
                    std::cerr << "Warning: fsync failed: " << strerror(errno) << std::endl;
                }
                // fd automatically closed by RAII
            }
        } catch (const std::exception& e) {
            if (callback) {
                WipeProgress progress{};
                progress.has_error = true;
                progress.error_message = "Wipe operation failed: " + std::string(e.what());
                callback(progress);
            }
            result = false;
        }

        // Send completion callback
        if (callback) {
            WipeProgress final_progress{};
            final_progress.is_complete = true;
            final_progress.has_error = !result;
            final_progress.percentage = result ? 100.0 : 0.0;

            if (state->cancel_requested.load()) {
                final_progress.status = "Operation cancelled";
                final_progress.has_error = true;
                final_progress.error_message = "Operation was cancelled by user";
            } else if (result) {
                final_progress.status = "Wipe completed successfully";
            } else if (!final_progress.has_error) {
                final_progress.has_error = true;
                final_progress.error_message = "Wipe operation failed";
            }

            callback(final_progress);
        }

        state->operation_in_progress.store(false);
    });

    return true;
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
