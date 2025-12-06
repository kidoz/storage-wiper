/**
 * @file Command.hpp
 * @brief Command pattern implementation for MVVM
 *
 * Provides command infrastructure for binding UI actions to ViewModel methods.
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mvvm {

/**
 * @class ICommand
 * @brief Interface for executable commands
 */
class ICommand {
public:
    virtual ~ICommand() = default;

    /**
     * @brief Check if the command can be executed
     * @return true if command can execute
     */
    [[nodiscard]] virtual auto can_execute() const -> bool = 0;

    /**
     * @brief Execute the command
     */
    virtual void execute() = 0;

    using CanExecuteChangedCallback = std::function<void()>;

    /**
     * @brief Subscribe to can_execute state changes
     * @param callback Function to call when can_execute changes
     * @return Subscription ID
     */
    virtual auto subscribe_can_execute_changed(CanExecuteChangedCallback callback) -> size_t = 0;

    /**
     * @brief Unsubscribe from can_execute changes
     * @param subscription_id ID returned from subscribe
     */
    virtual void unsubscribe_can_execute_changed(size_t subscription_id) = 0;
};

/**
 * @class RelayCommand
 * @brief Command implementation that delegates to callbacks
 *
 * @example
 * ```cpp
 * auto cmd = std::make_shared<RelayCommand>(
 *     [this]() { do_something(); },
 *     [this]() { return can_do_something_; }
 * );
 * if (cmd->can_execute()) {
 *     cmd->execute();
 * }
 * ```
 */
class RelayCommand : public ICommand {
public:
    using ExecuteCallback = std::function<void()>;
    using CanExecuteCallback = std::function<bool()>;

    /**
     * @brief Construct a command with execute and optional can_execute callbacks
     * @param execute Function to execute
     * @param can_execute Function to check if execution is allowed (default: always true)
     */
    explicit RelayCommand(ExecuteCallback execute,
                          CanExecuteCallback can_execute = []() { return true; })
        : execute_callback_(std::move(execute))
        , can_execute_callback_(std::move(can_execute)) {}

    [[nodiscard]] auto can_execute() const -> bool override {
        return can_execute_callback_ ? can_execute_callback_() : true;
    }

    void execute() override {
        if (can_execute() && execute_callback_) {
            execute_callback_();
        }
    }

    auto subscribe_can_execute_changed(CanExecuteChangedCallback callback) -> size_t override {
        std::lock_guard lock(mutex_);
        auto id = next_subscription_id_++;
        subscribers_[id] = std::move(callback);
        return id;
    }

    void unsubscribe_can_execute_changed(size_t subscription_id) override {
        std::lock_guard lock(mutex_);
        subscribers_.erase(subscription_id);
    }

    /**
     * @brief Notify subscribers that can_execute state may have changed
     *
     * Call this when conditions affecting can_execute() have changed.
     */
    void raise_can_execute_changed() {
        std::vector<CanExecuteChangedCallback> callbacks;
        {
            std::lock_guard lock(mutex_);
            callbacks.reserve(subscribers_.size());
            for (const auto& [id, callback] : subscribers_) {
                callbacks.push_back(callback);
            }
        }
        for (const auto& callback : callbacks) {
            callback();
        }
    }

private:
    ExecuteCallback execute_callback_;
    CanExecuteCallback can_execute_callback_;
    std::unordered_map<size_t, CanExecuteChangedCallback> subscribers_;
    size_t next_subscription_id_ = 0;
    mutable std::mutex mutex_;
};

/**
 * @class RelayCommand1
 * @brief Command with a single parameter
 *
 * @tparam T Parameter type
 */
template<typename T>
class RelayCommand1 {
public:
    using ExecuteCallback = std::function<void(const T&)>;
    using CanExecuteCallback = std::function<bool(const T&)>;

    explicit RelayCommand1(ExecuteCallback execute,
                           CanExecuteCallback can_execute = [](const T&) { return true; })
        : execute_callback_(std::move(execute))
        , can_execute_callback_(std::move(can_execute)) {}

    [[nodiscard]] auto can_execute(const T& param) const -> bool {
        return can_execute_callback_ ? can_execute_callback_(param) : true;
    }

    void execute(const T& param) {
        if (can_execute(param) && execute_callback_) {
            execute_callback_(param);
        }
    }

private:
    ExecuteCallback execute_callback_;
    CanExecuteCallback can_execute_callback_;
};

} // namespace mvvm
