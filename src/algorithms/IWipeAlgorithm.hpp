/**
 * @file IWipeAlgorithm.hpp
 * @brief Base interface for wipe algorithm implementations
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>

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
     * @brief Execute the wipe algorithm on a device by path
     *
     * This method allows algorithms like ATA Secure Erase to handle the device
     * directly instead of through a file descriptor. The default implementation
     * opens the device and calls execute().
     *
     * @param device_path Path to the device to wipe
     * @param size Size of the device in bytes
     * @param callback Progress callback function
     * @param cancel_flag Reference to cancellation flag
     * @return true if successful, false otherwise
     */
    virtual bool execute_on_device(const std::string& device_path, uint64_t size,
                                   ProgressCallback callback,
                                   const std::atomic<bool>& cancel_flag) {
        int fd = open(device_path.c_str(), O_WRONLY | O_SYNC);
        if (fd < 0) {
            if (callback) {
                WipeProgress progress{};
                progress.has_error = true;
                progress.error_message = "Failed to open device: " + device_path;
                progress.is_complete = true;
                callback(progress);
            }
            return false;
        }

        bool result = execute(fd, size, callback, cancel_flag);
        close(fd);
        return result;
    }

    /**
     * @brief Check if this algorithm requires device-level access
     *
     * If true, the WipeService should call execute_on_device() instead of
     * opening the device and calling execute(). This is needed for algorithms
     * like ATA Secure Erase that need to send specific commands to the device.
     *
     * @return true if device-level access is required
     */
    virtual bool requires_device_access() const { return false; }

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