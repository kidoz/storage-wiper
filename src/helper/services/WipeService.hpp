#pragma once

#include "services/IDiskService.hpp"
#include "services/IWipeService.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// Forward declaration
class IWipeAlgorithm;

class WipeService : public IWipeService {
public:
    explicit WipeService(std::shared_ptr<IDiskService> disk_service);
    ~WipeService() override;

    auto wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm, ProgressCallback callback)
        -> bool override;

    auto wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm, ProgressCallback callback,
                   bool verify) -> bool override;

    [[nodiscard]] auto supports_verification(WipeAlgorithm algo) -> bool override;
    [[nodiscard]] auto get_algorithm_name(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_algorithm_description(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_pass_count(WipeAlgorithm algo) -> int override;
    [[nodiscard]] auto is_ssd_compatible(WipeAlgorithm algo) -> bool override;
    auto cancel_operation(const std::string& device_path) -> bool override;

private:
    static constexpr auto SHUTDOWN_TIMEOUT = std::chrono::seconds{5};

    struct ThreadState {
        std::atomic<bool> cancel_requested{false};
        std::atomic<bool> operation_in_progress{false};
    };

    /**
     * @brief One wipe in flight, keyed by device path
     *
     * Each operation owns its thread and cancellation state so that wiping
     * several devices in parallel - and cancelling one of them - never
     * touches another operation. Completed entries stay in the map (thread
     * still joinable) until the same device is wiped again or the service is
     * destroyed; workers never mutate the map themselves.
     */
    struct Operation {
        std::shared_ptr<ThreadState> state = std::make_shared<ThreadState>();
        std::thread thread;
    };

    /**
     * @brief Result of wipe preparation validation
     */
    struct WipePreparation {
        std::shared_ptr<IWipeAlgorithm> algorithm;
        bool requires_device_access;
        std::shared_ptr<Operation> operation;
    };

    /**
     * @brief Result of wipe execution
     */
    struct WipeResult {
        bool success;
        uint64_t device_size;
    };

    std::shared_ptr<IDiskService> disk_service_;

    // One entry per device with a wipe in flight or recently finished
    std::map<std::string, std::shared_ptr<Operation>> operations_;
    mutable std::mutex operations_mutex_;  // Protects operations_

    // Algorithm factory
    std::map<WipeAlgorithm, std::shared_ptr<IWipeAlgorithm>> algorithms_;

    void initialize_algorithms();
    [[nodiscard]] auto get_algorithm(WipeAlgorithm algo) const -> std::shared_ptr<IWipeAlgorithm>;

public:
    /**
     * @brief Check whether a wipe is currently running on a device
     * @param disk_path Device to check
     */
    [[nodiscard]] auto is_operation_in_progress(const std::string& disk_path) -> bool;

private:
    /**
     * @brief Validate inputs and prepare for wipe operation
     * @param disk_path Path to the device
     * @param algorithm Wipe algorithm to use
     * @param callback Progress callback
     * @return WipePreparation if valid, nullopt if validation failed (error already reported)
     */
    [[nodiscard]] auto prepare_wipe(const std::string& disk_path, WipeAlgorithm algorithm,
                                    const ProgressCallback& callback)
        -> std::optional<WipePreparation>;

    /**
     * @brief Execute wipe on device (called from worker thread)
     * @param disk_path Path to the device
     * @param algorithm_ptr Algorithm to execute
     * @param requires_device_access Whether algorithm needs device-level access
     * @param tracked_callback Callback wrapped with progress tracker
     * @param state Thread state for cancellation
     * @return WipeResult with success status and device size
     */
    [[nodiscard]] static auto execute_wipe_on_device(
        const std::string& disk_path, const std::shared_ptr<IWipeAlgorithm>& algorithm_ptr,
        bool requires_device_access,
        const std::function<void(const WipeProgress&)>& tracked_callback,
        std::shared_ptr<ThreadState> state) -> WipeResult;

    /**
     * @brief Build completion status based on wipe and verification results
     * @param wipe_result Whether wipe succeeded
     * @param do_verify Whether verification was requested
     * @param verify_result Whether verification passed
     * @param cancelled Whether operation was cancelled
     * @param bad_blocks Number of sectors the final pass could not write
     * @param trim_issued Whether a BLKDISCARD was issued after the wipe
     * @return Completed WipeProgress struct
     */
    [[nodiscard]] static auto build_completion_status(bool wipe_result, bool do_verify,
                                                      bool verify_result, bool cancelled,
                                                      uint64_t bad_blocks, bool trim_issued)
        -> WipeProgress;
};
