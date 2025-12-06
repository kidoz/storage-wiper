/**
 * @file AlgorithmRegistry.hpp
 * @brief Auto-registration system for wipe algorithms
 *
 * Provides a registry pattern that allows algorithms to register themselves
 * automatically at static initialization time, eliminating manual registration.
 */

#pragma once

#include "algorithms/IWipeAlgorithm.hpp"
#include "interfaces/IWipeService.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace algorithms {

/**
 * @class AlgorithmRegistry
 * @brief Singleton registry for wipe algorithm factories
 *
 * Algorithms register themselves using the REGISTER_ALGORITHM macro.
 * The WipeService can then retrieve all registered algorithms without
 * manually maintaining a list.
 *
 * @example
 * ```cpp
 * // In ZeroFillAlgorithm.cpp
 * REGISTER_ALGORITHM(WipeAlgorithm::ZERO_FILL, ZeroFillAlgorithm);
 *
 * // In WipeService
 * auto algorithms = AlgorithmRegistry::instance().create_all();
 * ```
 */
class AlgorithmRegistry {
public:
    using FactoryFunc = std::function<std::shared_ptr<IWipeAlgorithm>()>;

    /**
     * @brief Get the singleton instance
     * @return Reference to the registry
     */
    [[nodiscard]] static auto instance() -> AlgorithmRegistry& {
        static AlgorithmRegistry registry;
        return registry;
    }

    /**
     * @brief Register an algorithm factory
     * @param algorithm The algorithm enum value
     * @param factory Factory function to create the algorithm
     */
    void register_algorithm(WipeAlgorithm algorithm, FactoryFunc factory) {
        std::lock_guard lock(mutex_);
        factories_[algorithm] = std::move(factory);
    }

    /**
     * @brief Create an instance of a specific algorithm
     * @param algorithm The algorithm to create
     * @return Shared pointer to the algorithm, or nullptr if not registered
     */
    [[nodiscard]] auto create(WipeAlgorithm algorithm) const
        -> std::shared_ptr<IWipeAlgorithm> {
        std::lock_guard lock(mutex_);
        auto it = factories_.find(algorithm);
        if (it != factories_.end()) {
            return it->second();
        }
        return nullptr;
    }

    /**
     * @brief Create instances of all registered algorithms
     * @return Map of algorithm enum to algorithm instance
     */
    [[nodiscard]] auto create_all() const
        -> std::map<WipeAlgorithm, std::shared_ptr<IWipeAlgorithm>> {
        std::lock_guard lock(mutex_);
        std::map<WipeAlgorithm, std::shared_ptr<IWipeAlgorithm>> result;
        for (const auto& [algo, factory] : factories_) {
            result[algo] = factory();
        }
        return result;
    }

    /**
     * @brief Get list of all registered algorithm types
     * @return Vector of registered algorithm enums
     */
    [[nodiscard]] auto get_registered_algorithms() const -> std::vector<WipeAlgorithm> {
        std::lock_guard lock(mutex_);
        std::vector<WipeAlgorithm> result;
        result.reserve(factories_.size());
        for (const auto& [algo, _] : factories_) {
            result.push_back(algo);
        }
        return result;
    }

    /**
     * @brief Check if an algorithm is registered
     * @param algorithm The algorithm to check
     * @return true if registered
     */
    [[nodiscard]] auto is_registered(WipeAlgorithm algorithm) const -> bool {
        std::lock_guard lock(mutex_);
        return factories_.contains(algorithm);
    }

    /**
     * @brief Get count of registered algorithms
     * @return Number of registered algorithms
     */
    [[nodiscard]] auto count() const -> size_t {
        std::lock_guard lock(mutex_);
        return factories_.size();
    }

private:
    AlgorithmRegistry() = default;
    ~AlgorithmRegistry() = default;

    AlgorithmRegistry(const AlgorithmRegistry&) = delete;
    AlgorithmRegistry& operator=(const AlgorithmRegistry&) = delete;

    std::map<WipeAlgorithm, FactoryFunc> factories_;
    mutable std::mutex mutex_;
};

/**
 * @class AlgorithmRegistrar
 * @brief Helper class for static registration of algorithms
 *
 * Used by the REGISTER_ALGORITHM macro to register an algorithm
 * at static initialization time.
 */
template<typename T>
class AlgorithmRegistrar {
public:
    explicit AlgorithmRegistrar(WipeAlgorithm algorithm) {
        AlgorithmRegistry::instance().register_algorithm(
            algorithm,
            []() -> std::shared_ptr<IWipeAlgorithm> {
                return std::make_shared<T>();
            }
        );
    }
};

} // namespace algorithms

/**
 * @def REGISTER_ALGORITHM
 * @brief Macro to register a wipe algorithm
 *
 * Place this macro in the .cpp file of each algorithm to automatically
 * register it with the AlgorithmRegistry.
 *
 * @param algo_enum The WipeAlgorithm enum value
 * @param algo_class The algorithm class name
 *
 * @example
 * ```cpp
 * // In ZeroFillAlgorithm.cpp
 * #include "algorithms/AlgorithmRegistry.hpp"
 *
 * REGISTER_ALGORITHM(WipeAlgorithm::ZERO_FILL, ZeroFillAlgorithm);
 *
 * // ... rest of implementation
 * ```
 */
#define REGISTER_ALGORITHM(algo_enum, algo_class) \
    namespace { \
        static ::algorithms::AlgorithmRegistrar<algo_class> \
            _registrar_##algo_class{algo_enum}; \
    }
