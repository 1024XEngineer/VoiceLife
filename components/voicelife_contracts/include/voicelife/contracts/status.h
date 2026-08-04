#pragma once

#include <optional>
#include <string>
#include <utility>

namespace voicelife {

/// 应用层和适配器操作返回的失败分类。
enum class ErrorCode {
    kNone = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kConflict,
    kUnavailable,
    kInternal,
};

/// 表示成功，或带有说明信息的类型化失败。
struct Status {
    ErrorCode code = ErrorCode::kNone;
    std::string message;

    /** @brief 判断状态是否成功。 @return code 为 kNone 时返回 true。 */
    [[nodiscard]] bool ok() const { return code == ErrorCode::kNone; }

    /** @brief 创建成功状态。 @return code 为 kNone 的状态。 */
    static Status Ok() { return {}; }
    /**
     * @brief 创建带错误分类的失败状态。
     * @param code 语义化失败分类。
     * @param message 面向人的失败说明。
     * @return 携带指定失败信息的状态。
     */
    static Status Error(ErrorCode code, std::string message) { return {code, std::move(message)}; }
};

/// 将操作状态与可选的成功值绑定在一起。
template <typename T>
struct Result {
    Status status;
    std::optional<T> value;

    /** @brief 判断状态和值是否同时表示成功。 @return 成功时返回 true。 */
    [[nodiscard]] bool ok() const { return status.ok() && value.has_value(); }

    /** @brief 创建成功结果。 @param value 要返回的值。 @return 携带值的成功结果。 */
    static Result Success(T value) { return {Status::Ok(), std::move(value)}; }
    /**
     * @brief 创建不携带值的失败结果。
     * @param code 语义化失败分类。
     * @param message 面向人的失败说明。
     * @return 不携带值的失败结果。
     */
    static Result Failure(ErrorCode code, std::string message) {
        return {Status::Error(code, std::move(message)), std::nullopt};
    }
};

/** @brief 返回稳定的错误码名称。 @param code 要描述的错误码。 @return 静态字符串形式的名称。 */
const char* ErrorCodeName(ErrorCode code);

}  // namespace voicelife
