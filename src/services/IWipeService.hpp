/**
 * @file IWipeService.hpp
 * @brief Interface for secure disk wiping operations
 *
 * This file defines the interface for secure disk wiping with multiple
 * DoD-compliant algorithms and progress reporting.
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <string>

/**
 * @class IWipeService
 * @brief Abstract interface for disk wiping operations
 */
class IWipeService {
public:
    virtual ~IWipeService() = default;

    /**
     * @brief Wipe a disk with the specified algorithm
     * @param disk_path Device path to wipe
     * @param algorithm Wipe algorithm to use
     * @param callback Progress callback
     * @return true if wipe started successfully
     */
    virtual auto wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                           ProgressCallback callback) -> bool = 0;

    /**
     * @brief Wipe a disk with optional verification
     * @param disk_path Device path to wipe
     * @param algorithm Wipe algorithm to use
     * @param callback Progress callback
     * @param verify If true, verify wipe by reading back data after writing
     * @return true if wipe started successfully
     */
    virtual auto wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                           ProgressCallback callback, bool verify) -> bool {
        // Default implementation ignores verify flag for backwards compatibility
        (void)verify;
        return wipe_disk(disk_path, algorithm, std::move(callback));
    }

    /**
     * @brief Check if an algorithm supports verification
     * @param algo Wipe algorithm
     * @return true if the algorithm can verify its wipe
     */
    [[nodiscard]] virtual auto supports_verification(WipeAlgorithm algo) -> bool {
        (void)algo;
        return false;
    }

    [[nodiscard]] virtual auto get_algorithm_name(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_algorithm_description(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_pass_count(WipeAlgorithm algo) -> int = 0;
    [[nodiscard]] virtual auto is_ssd_compatible(WipeAlgorithm algo) -> bool = 0;
    virtual auto cancel_current_operation() -> bool = 0;
};
