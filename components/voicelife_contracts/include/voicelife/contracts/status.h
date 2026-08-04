#pragma once

#include <optional>
#include <string>
#include <utility>

namespace voicelife {

/// Categorizes failures returned by application and adapter operations.
enum class ErrorCode {
    kNone = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kConflict,
    kUnavailable,
    kInternal,
};

/// Represents success or a typed failure with an explanatory message.
struct Status {
    ErrorCode code = ErrorCode::kNone;
    std::string message;

    /** @brief Reports whether this status is successful. @return `true` for `kNone`. */
    [[nodiscard]] bool ok() const { return code == ErrorCode::kNone; }

    /** @brief Creates a successful status. @return A status with error code `kNone`. */
    static Status Ok() { return {}; }
    /**
     * @brief Creates a typed failure status.
     * @param code Semantic failure category.
     * @param message Human-readable failure explanation.
     * @return A status carrying the supplied failure.
     */
    static Status Error(ErrorCode code, std::string message) { return {code, std::move(message)}; }
};

/// Couples an operation status with its optional successful value.
template <typename T>
struct Result {
    Status status;
    std::optional<T> value;

    /** @brief Reports whether both status and value indicate success. @return Success state. */
    [[nodiscard]] bool ok() const { return status.ok() && value.has_value(); }

    /** @brief Creates a successful result. @param value Returned value. @return Successful result. */
    static Result Success(T value) { return {Status::Ok(), std::move(value)}; }
    /**
     * @brief Creates a failed result without a value.
     * @param code Semantic failure category.
     * @param message Human-readable failure explanation.
     * @return Failed result without a value.
     */
    static Result Failure(ErrorCode code, std::string message) {
        return {Status::Error(code, std::move(message)), std::nullopt};
    }
};

/** @brief Returns a stable name for an error code. @param code Error code to describe. @return Static code name. */
const char* ErrorCodeName(ErrorCode code);

}  // namespace voicelife
