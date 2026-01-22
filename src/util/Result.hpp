/**
 * @file Result.hpp
 * @brief Result helpers based on std::expected
 */

#pragma once

#include <expected>
#include <string>
#include <utility>

namespace util {

/**
 * @struct Error
 * @brief Represents an error with a message and optional code
 */
struct Error {
    std::string message;
    int code = 0;

    Error() = default;
    explicit Error(std::string msg, int err_code = 0) : message(std::move(msg)), code(err_code) {}
};

/**
 * @brief Result alias using std::expected
 */
template <typename T>
using Result = std::expected<T, Error>;

}  // namespace util
