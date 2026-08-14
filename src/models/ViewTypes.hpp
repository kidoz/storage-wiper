/**
 * @file ViewTypes.hpp
 * @brief UI-specific data types for Views and ViewModels
 */

#pragma once

#include "models/WipeTypes.hpp"

#include <cstdint>
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

    // Monotonic per-message id. Observable::set() skips notification when the
    // new value compares equal, which would silently drop a repeated dialog
    // with identical text (and its callback); the sequence keeps each
    // show_message() distinct. Callbacks themselves are not comparable.
    uint64_t sequence = 0;

    auto operator==(const MessageInfo& other) const -> bool {
        return type == other.type && title == other.title && message == other.message &&
               sequence == other.sequence;
    }
};
