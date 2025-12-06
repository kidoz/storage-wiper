#pragma once

#include "interfaces/IWipeService.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <memory>
#include <map>
#include <sys/types.h>

// Forward declaration
class IWipeAlgorithm;

class WipeService : public IWipeService {
public:
    WipeService();
    ~WipeService() override;

    auto wipe_disk(const std::string& disk_path,
                   WipeAlgorithm algorithm,
                   ProgressCallback callback,
                   bool unmount_first = false) -> bool override;

    [[nodiscard]] auto get_algorithm_name(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_algorithm_description(WipeAlgorithm algo) -> std::string override;
    [[nodiscard]] auto get_pass_count(WipeAlgorithm algo) -> int override;
    [[nodiscard]] auto is_ssd_compatible(WipeAlgorithm algo) -> bool override;
    auto cancel_current_operation() -> bool override;

private:
    static constexpr auto SHUTDOWN_TIMEOUT = std::chrono::seconds{5};

    struct ThreadState {
        std::atomic<bool> cancel_requested{false};
        std::atomic<bool> operation_in_progress{false};
        std::atomic<pid_t> child_pid{0};  // PID of helper process for cancellation
    };

    std::shared_ptr<ThreadState> state_;
    std::thread wipe_thread_;
    mutable std::mutex thread_mutex_;  // Protects wipe_thread_ access

    // Algorithm factory (kept for algorithm info queries)
    std::map<WipeAlgorithm, std::shared_ptr<IWipeAlgorithm>> algorithms_;

    void initialize_algorithms();
    [[nodiscard]] auto get_algorithm(WipeAlgorithm algo) const -> std::shared_ptr<IWipeAlgorithm>;
};
