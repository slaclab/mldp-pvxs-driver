#pragma once

#include <string>
#include <stdexcept>
#include <optional>

namespace procmon {

/**
 * @brief Error type for Result
 */
struct Error {
    std::string message;
    explicit Error(std::string msg) : message(std::move(msg)) {}
};

/**
 * @brief A simple result type for C++20 (std::expected is C++23)
 * 
 * This is a lightweight alternative to std::expected that works with C++20.
 * It can hold either a value (T) or an error (Error).
 */
template<typename T>
class Result {
public:
    /**
     * @brief Construct a successful result
     */
    Result(T value) : value_(std::move(value)), error_(std::nullopt) {}
    
    /**
     * @brief Construct a failed result with error message
     */
    static Result error(std::string error_msg) {
        Result r;
        r.error_ = Error(std::move(error_msg));
        return r;
    }
    
    /**
     * @brief Check if result contains a value
     */
    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }
    
    /**
     * @brief Check if result is an error
     */
    [[nodiscard]] bool has_error() const noexcept {
        return error_.has_value();
    }
    
    /**
     * @brief Get the value (throws if error)
     */
    [[nodiscard]] const T& value() const & {
        if (has_error()) {
            throw std::runtime_error("Accessing value of error result");
        }
        return *value_;
    }
    
    /**
     * @brief Get the value (throws if error)
     */
    [[nodiscard]] T& value() & {
        if (has_error()) {
            throw std::runtime_error("Accessing value of error result");
        }
        return *value_;
    }
    
    /**
     * @brief Get the value (throws if error)
     */
    [[nodiscard]] T&& value() && {
        if (has_error()) {
            throw std::runtime_error("Accessing value of error result");
        }
        return std::move(*value_);
    }
    
    /**
     * @brief Get the error message (throws if not error)
     */
    [[nodiscard]] const std::string& error() const {
        if (!has_error()) {
            throw std::runtime_error("Accessing error of successful result");
        }
        return error_->message;
    }
    
    /**
     * @brief Dereference operator (for convenience)
     */
    [[nodiscard]] const T& operator*() const & { return value(); }
    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] T&& operator*() && { return std::move(value()); }
    
    /**
     * @brief Arrow operator (for convenience)
     */
    [[nodiscard]] const T* operator->() const { return &value(); }
    [[nodiscard]] T* operator->() { return &value(); }
    
    /**
     * @brief Bool conversion (true if has value)
     */
    explicit operator bool() const noexcept { return has_value(); }

private:
    Result() = default;
    std::optional<T> value_;
    std::optional<Error> error_;
};

/**
 * @brief Specialization for void (no value, just success/error)
 */
template<>
class Result<void> {
public:
    /**
     * @brief Construct a successful result
     */
    Result() : error_(std::nullopt) {}
    
    /**
     * @brief Construct a failed result
     */
    static Result error(std::string error_msg) {
        Result r;
        r.error_ = Error(std::move(error_msg));
        return r;
    }
    
    /**
     * @brief Check if result is successful
     */
    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }
    
    /**
     * @brief Check if result is an error
     */
    [[nodiscard]] bool has_error() const noexcept {
        return error_.has_value();
    }
    
    /**
     * @brief Get the error message (throws if not error)
     */
    [[nodiscard]] const std::string& error() const {
        if (!has_error()) {
            throw std::runtime_error("Accessing error of successful result");
        }
        return error_->message;
    }
    
    /**
     * @brief Bool conversion (true if successful)
     */
    explicit operator bool() const noexcept { return has_value(); }

private:
    std::optional<Error> error_;
};

} // namespace procmon
