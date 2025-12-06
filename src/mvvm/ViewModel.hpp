/**
 * @file ViewModel.hpp
 * @brief Base ViewModel class with child ViewModel support
 *
 * Provides infrastructure for hierarchical ViewModels in MVVM architecture.
 */

#pragma once

#include "mvvm/Observable.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mvvm {

/**
 * @class ViewModel
 * @brief Base class for ViewModels with child ViewModel support
 *
 * Provides common functionality for ViewModels including:
 * - Child ViewModel management
 * - Lifecycle management (initialize/cleanup)
 * - Property change notification inheritance
 *
 * @example
 * ```cpp
 * class SettingsViewModel : public mvvm::ViewModel {
 * public:
 *     Observable<bool> dark_mode{false};
 * };
 *
 * class MainViewModel : public mvvm::ViewModel {
 * public:
 *     MainViewModel() {
 *         add_child("settings", std::make_shared<SettingsViewModel>());
 *     }
 *
 *     auto settings() -> SettingsViewModel& {
 *         return *get_child<SettingsViewModel>("settings");
 *     }
 * };
 * ```
 */
class ViewModel : public ObservableObject {
public:
    ViewModel() = default;
    ~ViewModel() override {
        cleanup();
    }

    // Non-copyable, movable
    ViewModel(const ViewModel&) = delete;
    ViewModel& operator=(const ViewModel&) = delete;
    ViewModel(ViewModel&&) = default;
    ViewModel& operator=(ViewModel&&) = default;

    /**
     * @brief Initialize the ViewModel
     *
     * Override to perform initialization logic.
     * Called after construction and dependency injection.
     */
    virtual void initialize() {}

    /**
     * @brief Clean up the ViewModel
     *
     * Override to perform cleanup logic.
     * Called before destruction.
     */
    virtual void cleanup() {
        // Clean up children first
        for (auto& [name, child] : children_) {
            if (child) {
                child->cleanup();
            }
        }
        children_.clear();
    }

    /**
     * @brief Check if the ViewModel is initialized
     * @return true if initialized
     */
    [[nodiscard]] auto is_initialized() const -> bool {
        return initialized_;
    }

protected:
    /**
     * @brief Add a child ViewModel
     * @param name Unique name for the child
     * @param child The child ViewModel
     */
    void add_child(const std::string& name, std::shared_ptr<ViewModel> child) {
        children_[name] = std::move(child);
    }

    /**
     * @brief Remove a child ViewModel
     * @param name Name of the child to remove
     */
    void remove_child(const std::string& name) {
        if (auto it = children_.find(name); it != children_.end()) {
            if (it->second) {
                it->second->cleanup();
            }
            children_.erase(it);
        }
    }

    /**
     * @brief Get a child ViewModel by name
     * @tparam T The expected type of the child ViewModel
     * @param name Name of the child
     * @return Pointer to the child, or nullptr if not found
     */
    template<typename T>
    [[nodiscard]] auto get_child(const std::string& name) -> T* {
        static_assert(std::is_base_of_v<ViewModel, T>,
            "T must derive from ViewModel");

        auto it = children_.find(name);
        if (it != children_.end()) {
            return dynamic_cast<T*>(it->second.get());
        }
        return nullptr;
    }

    /**
     * @brief Get a child ViewModel by name (const version)
     * @tparam T The expected type of the child ViewModel
     * @param name Name of the child
     * @return Const pointer to the child, or nullptr if not found
     */
    template<typename T>
    [[nodiscard]] auto get_child(const std::string& name) const -> const T* {
        static_assert(std::is_base_of_v<ViewModel, T>,
            "T must derive from ViewModel");

        auto it = children_.find(name);
        if (it != children_.end()) {
            return dynamic_cast<const T*>(it->second.get());
        }
        return nullptr;
    }

    /**
     * @brief Check if a child exists
     * @param name Name of the child
     * @return true if child exists
     */
    [[nodiscard]] auto has_child(const std::string& name) const -> bool {
        return children_.contains(name);
    }

    /**
     * @brief Mark the ViewModel as initialized
     */
    void set_initialized() {
        initialized_ = true;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<ViewModel>> children_;
    bool initialized_ = false;
};

} // namespace mvvm
