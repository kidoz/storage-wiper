/**
 * @file IDiskService.hpp
 * @brief Interface for disk management operations
 *
 * This file defines the interface for secure disk operations including
 * discovery, validation, and basic management functions.
 */

#pragma once

#include "models/DiskInfo.hpp"
#include "util/Result.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

/**
 * @class IDiskService
 * @brief Abstract interface for disk management operations
 *
 * This interface provides methods for discovering, validating, and managing
 * storage devices in a secure manner. All implementations must ensure proper
 * validation to prevent unauthorized access to system devices.
 */
class IDiskService {
public:
    virtual ~IDiskService() = default;

    /**
     * @brief Get list of available storage devices
     * @return Vector of DiskInfo structures
     */
    [[nodiscard]] virtual auto get_available_disks() -> std::vector<DiskInfo> = 0;

    /**
     * @brief Safely unmount a disk
     * @param path Device path to unmount
     * @return True if successful, false otherwise
     */
    virtual auto unmount_disk(const std::string& path) -> std::expected<void, util::Error> = 0;

    /**
     * @brief Check if disk is writable
     * @param path Device path to check
     * @return True if writable, false otherwise
     */
    [[nodiscard]] virtual auto is_disk_writable(const std::string& path) -> bool = 0;

    /**
     * @brief Get disk size in bytes
     * @param path Device path
     * @return Size in bytes, 0 on error
     */
    [[nodiscard]] virtual auto get_disk_size(const std::string& path)
        -> std::expected<uint64_t, util::Error> = 0;

    /**
     * @brief Validate device path for security
     * @param path Device path to validate
     * @return True if path is valid and safe, false otherwise
     */
    [[nodiscard]] virtual auto validate_device_path(const std::string& path)
        -> std::expected<void, util::Error> = 0;
};
