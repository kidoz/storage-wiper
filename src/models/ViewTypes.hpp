/**
 * @file ViewTypes.hpp
 * @brief UI-specific data types for Views and ViewModels
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <functional>
#include <string>

/**
 * @struct AlgorithmInfo
 * @brief Information about a wipe algorithm for UI display
 */
struct AlgorithmInfo {
    WipeAlgorithm algorithm = WipeAlgorithm::ZERO_FILL;
    std::string name;
    std::string description;
    int pass_count = 0;
    bool is_ssd_compatible = false;

    auto operator==(const AlgorithmInfo&) const -> bool = default;
};

/**
 * @struct MessageInfo
 * @brief Information for displaying messages to the user
 */
struct MessageInfo {
    enum class Type {
        INFO,
        ERROR,
        CONFIRMATION
    };
    Type type = Type::INFO;
    std::string title;
    std::string message;
    std::function<void(bool)> confirmation_callback;

    // Note: Comparing callbacks is not possible, so we compare by content only
    auto operator==(const MessageInfo& other) const -> bool {
        return type == other.type && title == other.title && message == other.message;
    }
};
