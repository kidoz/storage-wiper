/**
 * @file Observable.hpp
 * @brief Observable property system for MVVM data binding
 *
 * Provides a type-safe observable property implementation with
 * change notification support for MVVM architecture.
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mvvm {

/**
 * @class IPropertyChanged
 * @brief Interface for objects that notify property changes
 */
class IPropertyChanged {
public:
    virtual ~IPropertyChanged() = default;

    using PropertyChangedCallback = std::function<void(const std::string&)>;

    /**
     * @brief Subscribe to property change notifications
     * @param callback Function to call when any property changes
     * @return Subscription ID for unsubscribing
     */
    virtual auto subscribe(PropertyChangedCallback callback) -> size_t = 0;

    /**
     * @brief Unsubscribe from property change notifications
     * @param subscription_id ID returned from subscribe()
     */
    virtual void unsubscribe(size_t subscription_id) = 0;
};

/**
 * @class ObservableObject
 * @brief Base class for objects with observable properties
 *
 * Provides property change notification infrastructure for ViewModels.
 */
class ObservableObject : public IPropertyChanged {
public:
    ObservableObject() = default;
    ~ObservableObject() override = default;

    // Non-copyable and non-movable due to mutex_ member
    ObservableObject(const ObservableObject&) = delete;
    ObservableObject& operator=(const ObservableObject&) = delete;
    ObservableObject(ObservableObject&&) = delete;
    ObservableObject& operator=(ObservableObject&&) = delete;

    auto subscribe(PropertyChangedCallback callback) -> size_t override {
        std::lock_guard lock(mutex_);
        auto id = next_subscription_id_++;
        subscribers_[id] = std::move(callback);
        return id;
    }

    void unsubscribe(size_t subscription_id) override {
        std::lock_guard lock(mutex_);
        subscribers_.erase(subscription_id);
    }

protected:
    /**
     * @brief Notify all subscribers that a property has changed
     * @param property_name Name of the changed property
     */
    void notify_property_changed(const std::string& property_name) {
        std::vector<PropertyChangedCallback> callbacks;
        {
            std::lock_guard lock(mutex_);
            callbacks.reserve(subscribers_.size());
            for (const auto& [id, callback] : subscribers_) {
                callbacks.push_back(callback);
            }
        }
        for (const auto& callback : callbacks) {
            callback(property_name);
        }
    }

    /**
     * @brief Set a property value and notify if changed
     * @tparam T Property type
     * @param field Reference to the field to set
     * @param value New value
     * @param property_name Name of the property for notification
     * @return true if the value changed, false otherwise
     */
    template <typename T>
    auto set_property(T& field, const T& value, const std::string& property_name) -> bool {
        if (field == value) {
            return false;
        }
        field = value;
        notify_property_changed(property_name);
        return true;
    }

private:
    std::unordered_map<size_t, PropertyChangedCallback> subscribers_;
    size_t next_subscription_id_ = 0;
    mutable std::mutex mutex_;
};

/**
 * @class Observable
 * @brief Type-safe observable property wrapper
 *
 * @tparam T The type of the observable value
 *
 * @example
 * ```cpp
 * Observable<std::string> name{"Initial"};
 * name.subscribe([](const std::string& new_value) {
 *     std::cout << "Name changed to: " << new_value << std::endl;
 * });
 * name.set("New Name");  // Triggers callback
 * ```
 */
template <typename T>
class Observable {
public:
    using ValueChangedCallback = std::function<void(const T&)>;

    explicit Observable(T initial_value = T{}) : value_(std::move(initial_value)) {}

    /**
     * @brief Get the current value
     */
    [[nodiscard]] auto get() const -> const T& {
        std::lock_guard lock(mutex_);
        return value_;
    }

    /**
     * @brief Set a new value and notify subscribers if changed
     * @param new_value The new value to set
     * @return true if value changed, false if same
     */
    auto set(const T& new_value) -> bool {
        {
            std::lock_guard lock(mutex_);
            if (value_ == new_value) {
                return false;
            }
            value_ = new_value;
        }
        notify_subscribers();
        return true;
    }

    /**
     * @brief Subscribe to value changes
     * @param callback Function called with new value when changed
     * @return Subscription ID
     */
    auto subscribe(ValueChangedCallback callback) -> size_t {
        std::lock_guard lock(mutex_);
        auto id = next_id_++;
        subscribers_[id] = std::move(callback);
        return id;
    }

    /**
     * @brief Unsubscribe from value changes
     * @param id Subscription ID returned from subscribe()
     */
    void unsubscribe(size_t id) {
        std::lock_guard lock(mutex_);
        subscribers_.erase(id);
    }

    /**
     * @brief Explicit conversion to underlying type
     * @note Made explicit to prevent unintended copies. Use get() for direct access.
     */
    explicit operator const T&() const { return get(); }

private:
    void notify_subscribers() {
        std::vector<ValueChangedCallback> callbacks;
        T current_value;
        {
            std::lock_guard lock(mutex_);
            callbacks.reserve(subscribers_.size());
            for (const auto& [id, callback] : subscribers_) {
                callbacks.push_back(callback);
            }
            current_value = value_;
        }
        for (const auto& callback : callbacks) {
            callback(current_value);
        }
    }

    T value_;
    std::unordered_map<size_t, ValueChangedCallback> subscribers_;
    size_t next_id_ = 0;
    mutable std::mutex mutex_;
};

}  // namespace mvvm
