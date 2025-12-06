/**
 * @file IWipeAlgorithm.hpp
 * @brief Base interface for wipe algorithm implementations
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <atomic>

// Forward declarations - actual definitions in IWipeService.hpp
struct WipeProgress;
using ProgressCallback = std::function<void(const WipeProgress&)>;

/**
 * @class IWipeAlgorithm
 * @brief Interface for disk wiping algorithm implementations
 */
class IWipeAlgorithm {
public:
    virtual ~IWipeAlgorithm() = default;

    /**
     * @brief Execute the wipe algorithm on the specified file descriptor
     * @param fd File descriptor of the device to wipe
     * @param size Size of the device in bytes
     * @param callback Progress callback function
     * @param cancel_flag Reference to cancellation flag
     * @return true if successful, false otherwise
     */
    virtual bool execute(int fd, uint64_t size, ProgressCallback callback,
                        const std::atomic<bool>& cancel_flag) = 0;

    /**
     * @brief Get the name of this algorithm
     * @return Algorithm name
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief Get a description of this algorithm
     * @return Algorithm description
     */
    virtual std::string get_description() const = 0;

    /**
     * @brief Get the number of passes this algorithm performs
     * @return Number of passes
     */
    virtual int get_pass_count() const = 0;

    /**
     * @brief Check if this algorithm is compatible with SSDs
     * @return true if SSD compatible, false otherwise
     */
    virtual bool is_ssd_compatible() const = 0;
};