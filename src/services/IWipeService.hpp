/**
 * @file IWipeService.hpp
 * @brief Interface for secure disk wiping operations
 *
 * This file defines the interface for secure disk wiping with multiple
 * DoD-compliant algorithms and progress reporting.
 */

#pragma once

#include "models/WipeJobSnapshot.hpp"
#include "models/WipePlan.hpp"
#include "models/WipeTypes.hpp"
#include "util/Result.hpp"

#include <string>

/**
 * @class IWipeService
 * @brief Abstract interface for disk wiping operations
 */
class IWipeService {
public:
    virtual ~IWipeService() = default;

    [[nodiscard]] virtual auto prepare_wipe(const std::string&, WipeAlgorithm, bool)
        -> std::expected<WipePlan, util::Error> {
        return std::unexpected(util::Error{"Prepared wipes are not supported by this service"});
    }

    virtual auto start_prepared_wipe(const WipePlan&, ProgressCallback) -> bool { return false; }

    virtual void discard_wipe_plan(const std::string&) {}

    [[nodiscard]] virtual auto get_wipe_jobs()
        -> std::expected<std::vector<WipeJobSnapshot>, util::Error> {
        return std::vector<WipeJobSnapshot>{};
    }
    virtual auto observe_wipe_job(const WipePlan&, ProgressCallback) -> bool { return false; }
    virtual auto cancel_prepared_wipe(const std::string&) -> bool { return false; }

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

    /**
     * @brief Get the NIST SP 800-88 sanitization category of an algorithm
     * @param algo Wipe algorithm
     * @return Category label, e.g. "NIST 800-88 Clear" or "NIST 800-88 Purge"
     */
    [[nodiscard]] virtual auto get_nist_category(WipeAlgorithm algo) -> std::string {
        return std::string{nist_800_88_category(algo)};
    }

    /**
     * @brief Request cancellation of the wipe running on a specific device
     * @param device_path Device whose wipe should be cancelled
     * @return true if a wipe was running on the device and cancellation was
     *         requested; the operation still finishes asynchronously
     */
    virtual auto cancel_operation(const std::string& device_path) -> bool = 0;

    [[nodiscard]] virtual auto get_algorithm_name(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_algorithm_description(WipeAlgorithm algo) -> std::string = 0;
    [[nodiscard]] virtual auto get_pass_count(WipeAlgorithm algo) -> int = 0;
    [[nodiscard]] virtual auto is_ssd_compatible(WipeAlgorithm algo) -> bool = 0;
};
