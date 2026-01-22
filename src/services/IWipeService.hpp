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

    virtual auto wipe_disk(const std::string& disk_path, WipeAlgorithm algorithm,
                           ProgressCallback callback) -> bool = 0;

    [[nodiscard]] virtual auto get_algorithm_name(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_algorithm_description(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_pass_count(WipeAlgorithm algo) -> int = 0;
    [[nodiscard]] virtual auto is_ssd_compatible(WipeAlgorithm algo) -> bool = 0;
    virtual auto cancel_current_operation() -> bool = 0;
};
