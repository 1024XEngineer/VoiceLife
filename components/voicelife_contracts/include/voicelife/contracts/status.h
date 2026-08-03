#pragma once

#include <optional>
#include <string>
#include <utility>

namespace voicelife {

enum class ErrorCode {
    kNone = 0,
    kInvalidArgument,
    kNotFound,
    kConflict,
    kUnavailable,
    kInternal,
};

struct Status {
    ErrorCode code = ErrorCode::kNone;
    std::string message;

    [[nodiscard]] bool ok() const { return code == ErrorCode::kNone; }

    static Status Ok() { return {}; }
    static Status Error(ErrorCode code, std::string message) { return {code, std::move(message)}; }
};

template <typename T>
struct Result {
    Status status;
    std::optional<T> value;

    [[nodiscard]] bool ok() const { return status.ok() && value.has_value(); }

    static Result Success(T value) { return {Status::Ok(), std::move(value)}; }
    static Result Failure(ErrorCode code, std::string message) {
        return {Status::Error(code, std::move(message)), std::nullopt};
    }
};

const char* ErrorCodeName(ErrorCode code);

}  // namespace voicelife
