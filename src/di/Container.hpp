/**
 * @file Container.hpp
 * @brief Lightweight dependency injection container
 *
 * A type-safe, header-only DI container for managing service dependencies.
 * Supports singleton and transient lifetimes with interface-based resolution.
 */

#pragma once

#include <any>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace di {

/**
 * @enum Lifetime
 * @brief Specifies the lifetime of a registered service
 */
enum class Lifetime {
    SINGLETON,  ///< Single instance shared across all resolutions
    TRANSIENT   ///< New instance created on each resolution
};

/**
 * @class Container
 * @brief Lightweight dependency injection container
 *
 * Manages service registration and resolution with support for
 * interface-based dependency injection and lifetime management.
 *
 * @example
 * ```cpp
 * di::Container container;
 * container.register_type<IDiskService, DiskService>();
 * container.register_type<IWipeService, WipeService>();
 * auto disk_service = container.resolve<IDiskService>();
 * ```
 */
class Container {
public:
    Container() = default;
    ~Container() = default;

    // Non-copyable, non-movable (singleton container pattern)
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) = delete;
    Container& operator=(Container&&) = delete;

    /**
     * @brief Register a type mapping from interface to implementation
     * @tparam Interface The interface type to register
     * @tparam Implementation The concrete implementation type
     * @param lifetime Service lifetime (SINGLETON or TRANSIENT)
     *
     * @example
     * ```cpp
     * container.register_type<IDiskService, DiskService>(Lifetime::SINGLETON);
     * ```
     */
    template<typename Interface, typename Implementation>
    void register_type(Lifetime lifetime = Lifetime::SINGLETON) {
        static_assert(std::is_base_of_v<Interface, Implementation>,
            "Implementation must derive from Interface");

        auto factory = []() -> std::shared_ptr<Interface> {
            return std::make_shared<Implementation>();
        };

        std::lock_guard lock(mutex_);
        registrations_[std::type_index(typeid(Interface))] = Registration{
            .factory = [factory]() -> std::any { return factory(); },
            .lifetime = lifetime,
            .instance = std::any{}
        };
    }

    /**
     * @brief Register a type with a custom factory function
     * @tparam Interface The interface type to register
     * @param factory Factory function that creates the implementation
     * @param lifetime Service lifetime (SINGLETON or TRANSIENT)
     *
     * @example
     * ```cpp
     * container.register_factory<IDiskService>(
     *     [&]() { return std::make_shared<DiskService>(config); },
     *     Lifetime::SINGLETON
     * );
     * ```
     */
    template<typename Interface>
    void register_factory(std::function<std::shared_ptr<Interface>()> factory,
                          Lifetime lifetime = Lifetime::SINGLETON) {
        std::lock_guard lock(mutex_);
        registrations_[std::type_index(typeid(Interface))] = Registration{
            .factory = [factory]() -> std::any { return factory(); },
            .lifetime = lifetime,
            .instance = std::any{}
        };
    }

    /**
     * @brief Register an existing instance as a singleton
     * @tparam Interface The interface type to register
     * @param instance The pre-created instance to register
     *
     * @example
     * ```cpp
     * auto service = std::make_shared<DiskService>();
     * container.register_instance<IDiskService>(service);
     * ```
     */
    template<typename Interface>
    void register_instance(std::shared_ptr<Interface> instance) {
        std::lock_guard lock(mutex_);
        registrations_[std::type_index(typeid(Interface))] = Registration{
            .factory = nullptr,
            .lifetime = Lifetime::SINGLETON,
            .instance = instance
        };
    }

    /**
     * @brief Resolve a registered service
     * @tparam Interface The interface type to resolve
     * @return Shared pointer to the resolved service
     * @throws std::runtime_error if the type is not registered
     *
     * @example
     * ```cpp
     * auto service = container.resolve<IDiskService>();
     * ```
     */
    template<typename Interface>
    [[nodiscard]] auto resolve() -> std::shared_ptr<Interface> {
        std::lock_guard lock(mutex_);

        auto type_id = std::type_index(typeid(Interface));
        auto it = registrations_.find(type_id);

        if (it == registrations_.end()) {
            throw std::runtime_error(
                std::string("Type not registered: ") + typeid(Interface).name());
        }

        auto& registration = it->second;

        // Return existing singleton instance if available
        if (registration.lifetime == Lifetime::SINGLETON &&
            registration.instance.has_value()) {
            return std::any_cast<std::shared_ptr<Interface>>(registration.instance);
        }

        // Create new instance
        if (!registration.factory) {
            throw std::runtime_error(
                std::string("No factory registered for type: ") + typeid(Interface).name());
        }

        auto instance = std::any_cast<std::shared_ptr<Interface>>(registration.factory());

        // Cache singleton instances
        if (registration.lifetime == Lifetime::SINGLETON) {
            registration.instance = instance;
        }

        return instance;
    }

    /**
     * @brief Check if a type is registered
     * @tparam Interface The interface type to check
     * @return true if the type is registered
     */
    template<typename Interface>
    [[nodiscard]] auto is_registered() const -> bool {
        std::lock_guard lock(mutex_);
        return registrations_.contains(std::type_index(typeid(Interface)));
    }

    /**
     * @brief Clear all registrations and cached instances
     */
    void clear() {
        std::lock_guard lock(mutex_);
        registrations_.clear();
    }

    /**
     * @brief Get the number of registered types
     * @return Number of registered types
     */
    [[nodiscard]] auto size() const -> size_t {
        std::lock_guard lock(mutex_);
        return registrations_.size();
    }

private:
    struct Registration {
        std::function<std::any()> factory;
        Lifetime lifetime;
        std::any instance;
    };

    std::unordered_map<std::type_index, Registration> registrations_;
    mutable std::mutex mutex_;
};

/**
 * @class ServiceLocator
 * @brief Global service locator for static access to a shared container
 *
 * @deprecated Prefer constructor injection via Container. ServiceLocator
 * creates hidden dependencies and makes code harder to test. This class
 * is retained only for legacy compatibility and edge cases where DI
 * container access is truly needed (e.g., integration tests, plugins).
 *
 * **Preferred approach:**
 * ```cpp
 * // Constructor injection (recommended)
 * class MyService {
 *     std::shared_ptr<IDiskService> disk_service_;
 * public:
 *     explicit MyService(std::shared_ptr<IDiskService> disk_service)
 *         : disk_service_(std::move(disk_service)) {}
 * };
 *
 * // Registration
 * container.register_factory<IMyService>([&container]() {
 *     return std::make_shared<MyService>(container.resolve<IDiskService>());
 * });
 * ```
 *
 * **Avoid:**
 * ```cpp
 * // Service locator anti-pattern
 * class MyService {
 *     void do_something() {
 *         auto disk = ServiceLocator::resolve<IDiskService>(); // Hidden dependency!
 *     }
 * };
 * ```
 */
class [[deprecated("Prefer constructor injection via Container")]] ServiceLocator {
public:
    ServiceLocator() = delete;

    /**
     * @brief Get the global container instance
     * @return Reference to the global container
     * @deprecated Use constructor injection instead
     */
    [[nodiscard]] static auto instance() -> Container& {
        static Container container;
        return container;
    }

    /**
     * @brief Configure the global container
     * @param configurator Function that registers services
     * @deprecated Configure the Container directly in your composition root
     */
    static void configure(const std::function<void(Container&)>& configurator) {
        configurator(instance());
    }

    /**
     * @brief Resolve a service from the global container
     * @tparam Interface The interface type to resolve
     * @return Shared pointer to the resolved service
     * @deprecated Use constructor injection instead
     */
    template<typename Interface>
    [[nodiscard]] static auto resolve() -> std::shared_ptr<Interface> {
        return instance().resolve<Interface>();
    }

    /**
     * @brief Reset the global container (primarily for testing)
     */
    static void reset() {
        instance().clear();
    }
};

} // namespace di
