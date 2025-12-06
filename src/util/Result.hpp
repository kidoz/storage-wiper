/**
 * @file Result.hpp
 * @brief Result type for consistent error handling
 *
 * Provides a type-safe way to return either a success value or an error,
 * avoiding the mix of exceptions and error codes.
 */

#pragma once

#include <optional>
#include <string>
#include <variant>

namespace util {

/**
 * @struct Error
 * @brief Represents an error with a message and optional code
 */
struct Error {
    std::string message;
    int code = 0;

    Error() = default;
    explicit Error(std::string msg, int err_code = 0)
        : message(std::move(msg)), code(err_code) {}

    [[nodiscard]] auto what() const -> const std::string& {
        return message;
    }
};

/**
 * @class Result
 * @brief A type that holds either a success value or an error
 *
 * @tparam T The success value type
 *
 * @example
 * ```cpp
 * auto divide(int a, int b) -> Result<int> {
 *     if (b == 0) {
 *         return Result<int>::error("Division by zero");
 *     }
 *     return Result<int>::ok(a / b);
 * }
 *
 * auto result = divide(10, 2);
 * if (result) {
 *     std::cout << "Result: " << result.value() << std::endl;
 * } else {
 *     std::cerr << "Error: " << result.error().message << std::endl;
 * }
 * ```
 */
template<typename T>
class Result {
public:
    /**
     * @brief Create a success result
     * @param value The success value
     * @return Result containing the value
     */
    [[nodiscard]] static auto ok(T value) -> Result {
        Result r;
        r.data_ = std::move(value);
        return r;
    }

    /**
     * @brief Create an error result
     * @param message Error message
     * @param code Optional error code
     * @return Result containing the error
     */
    [[nodiscard]] static auto error(std::string message, int code = 0) -> Result {
        Result r;
        r.data_ = Error{std::move(message), code};
        return r;
    }

    /**
     * @brief Create an error result from an Error object
     * @param err The error object
     * @return Result containing the error
     */
    [[nodiscard]] static auto error(Error err) -> Result {
        Result r;
        r.data_ = std::move(err);
        return r;
    }

    /**
     * @brief Check if result is success
     * @return true if result holds a value
     */
    [[nodiscard]] auto is_ok() const -> bool {
        return std::holds_alternative<T>(data_);
    }

    /**
     * @brief Check if result is error
     * @return true if result holds an error
     */
    [[nodiscard]] auto is_error() const -> bool {
        return std::holds_alternative<Error>(data_);
    }

    /**
     * @brief Boolean conversion (true if success)
     */
    explicit operator bool() const {
        return is_ok();
    }

    /**
     * @brief Get the success value
     * @return Reference to the value
     * @throws std::bad_variant_access if result is an error
     */
    [[nodiscard]] auto value() -> T& {
        return std::get<T>(data_);
    }

    /**
     * @brief Get the success value (const)
     * @return Const reference to the value
     * @throws std::bad_variant_access if result is an error
     */
    [[nodiscard]] auto value() const -> const T& {
        return std::get<T>(data_);
    }

    /**
     * @brief Get the value or a default
     * @param default_value Value to return if result is error
     * @return The value or default
     */
    [[nodiscard]] auto value_or(T default_value) const -> T {
        if (is_ok()) {
            return std::get<T>(data_);
        }
        return default_value;
    }

    /**
     * @brief Get the error
     * @return Reference to the error
     * @throws std::bad_variant_access if result is success
     */
    [[nodiscard]] auto get_error() -> Error& {
        return std::get<Error>(data_);
    }

    /**
     * @brief Get the error (const)
     * @return Const reference to the error
     * @throws std::bad_variant_access if result is success
     */
    [[nodiscard]] auto get_error() const -> const Error& {
        return std::get<Error>(data_);
    }

    /**
     * @brief Map the success value to a new type
     * @tparam U The new value type
     * @param fn Function to transform the value
     * @return New Result with transformed value or same error
     */
    template<typename U>
    [[nodiscard]] auto map(std::function<U(const T&)> fn) const -> Result<U> {
        if (is_ok()) {
            return Result<U>::ok(fn(std::get<T>(data_)));
        }
        return Result<U>::error(std::get<Error>(data_));
    }

    /**
     * @brief Execute function if success, chain Results
     * @tparam U The new value type
     * @param fn Function that returns a Result<U>
     * @return Result from fn or error
     */
    template<typename U>
    [[nodiscard]] auto and_then(std::function<Result<U>(const T&)> fn) const -> Result<U> {
        if (is_ok()) {
            return fn(std::get<T>(data_));
        }
        return Result<U>::error(std::get<Error>(data_));
    }

private:
    Result() = default;
    std::variant<T, Error> data_;
};

/**
 * @brief Specialization for void (operations that can fail but return nothing)
 */
template<>
class Result<void> {
public:
    /**
     * @brief Create a success result
     * @return Success Result
     */
    [[nodiscard]] static auto ok() -> Result {
        Result r;
        r.success_ = true;
        return r;
    }

    /**
     * @brief Create an error result
     * @param message Error message
     * @param code Optional error code
     * @return Result containing the error
     */
    [[nodiscard]] static auto error(std::string message, int code = 0) -> Result {
        Result r;
        r.success_ = false;
        r.error_ = Error{std::move(message), code};
        return r;
    }

    /**
     * @brief Create an error result from an Error object
     * @param err The error object
     * @return Result containing the error
     */
    [[nodiscard]] static auto error(Error err) -> Result {
        Result r;
        r.success_ = false;
        r.error_ = std::move(err);
        return r;
    }

    [[nodiscard]] auto is_ok() const -> bool { return success_; }
    [[nodiscard]] auto is_error() const -> bool { return !success_; }
    explicit operator bool() const { return success_; }

    [[nodiscard]] auto get_error() const -> const Error& { return error_; }

private:
    Result() = default;
    bool success_ = false;
    Error error_;
};

} // namespace util
