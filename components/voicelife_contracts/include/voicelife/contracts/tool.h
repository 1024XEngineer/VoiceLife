#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <variant>

#include "voicelife/contracts/json.h"
#include "voicelife/contracts/status.h"

namespace voicelife {

/// 工具调用参数当前支持的运行时值类型。
using ToolValue = std::variant<bool, int64_t, std::string, JsonValue>;
using ToolArguments = std::unordered_map<std::string, ToolValue>;

/// 描述一次进入设备侧的工具调用。
struct ToolCall {
    std::string request_id;
    std::string name;
    ToolArguments arguments;
};

/// 工具返回的结构化 JSON 值。
struct ToolOutputValue;

/// 工具返回的数组元素集合。
using ToolOutputArray = std::vector<std::shared_ptr<ToolOutputValue>>;

/// 工具返回的对象成员集合；使用 vector 保持业务声明顺序。
using ToolOutputObject = std::vector<std::pair<std::string, std::shared_ptr<ToolOutputValue>>>;

/// 工具返回的结构化 JSON 值。
struct ToolOutputValue {
    enum class Kind { kNull, kBoolean, kInteger, kString, kArray, kObject };

    Kind kind = Kind::kNull;
    bool boolean = false;
    std::int64_t integer = 0;
    std::string string;
    std::shared_ptr<ToolOutputArray> array;
    std::shared_ptr<ToolOutputObject> object;

    /** @brief 构造空值。 @return 空值节点。 */
    static ToolOutputValue Null() { return {}; }
    /** @brief 构造布尔值节点。 @param value 布尔值。 @return 布尔节点。 */
    static ToolOutputValue Boolean(bool value) {
        ToolOutputValue output;
        output.kind = Kind::kBoolean;
        output.boolean = value;
        return output;
    }
    /** @brief 构造整数节点。 @param value 整数值。 @return 整数节点。 */
    static ToolOutputValue Integer(std::int64_t value) {
        ToolOutputValue output;
        output.kind = Kind::kInteger;
        output.integer = value;
        return output;
    }
    /** @brief 构造字符串节点。 @param value 字符串。 @return 字符串节点。 */
    static ToolOutputValue String(std::string value) {
        ToolOutputValue output;
        output.kind = Kind::kString;
        output.string = std::move(value);
        return output;
    }
    /** @brief 构造数组节点。 @param value 数组元素。 @return 数组节点。 */
    static ToolOutputValue Array(ToolOutputArray value) {
        ToolOutputValue output;
        output.kind = Kind::kArray;
        output.array = std::make_shared<ToolOutputArray>(std::move(value));
        return output;
    }
    /** @brief 构造对象节点。 @param value 有序成员。 @return 对象节点。 */
    static ToolOutputValue Object(ToolOutputObject value) {
        ToolOutputValue output;
        output.kind = Kind::kObject;
        output.object = std::make_shared<ToolOutputObject>(std::move(value));
        return output;
    }

    /** @brief 判断当前值是否为对象。 @return 是对象时返回 true。 */
    [[nodiscard]] bool IsObject() const { return kind == Kind::kObject; }
    /** @brief 判断当前值是否为数组。 @return 是数组时返回 true。 */
    [[nodiscard]] bool IsArray() const { return kind == Kind::kArray; }
    /** @brief 判断当前值是否为字符串。 @return 是字符串时返回 true。 */
    [[nodiscard]] bool IsString() const { return kind == Kind::kString; }
};

/** @brief 创建工具输出数组中的一个元素。 @param value 节点。 @return 节点共享指针。 */
inline std::shared_ptr<ToolOutputValue> MakeToolOutput(ToolOutputValue value) {
    return std::make_shared<ToolOutputValue>(std::move(value));
}

/** @brief 创建工具输出对象成员。 @param key 成员名。 @param value 节点。 @return 有序成员。 */
inline std::pair<std::string, std::shared_ptr<ToolOutputValue>> MakeToolOutput(std::string key, ToolOutputValue value) {
    return {std::move(key), MakeToolOutput(std::move(value))};
}

/// 保存工具调用的状态和结构化输出值。
struct ToolResult {
    Status status;
    ToolOutputValue output = ToolOutputValue::Null();
    /// 面向用户的精确文本；未设置时由边界适配器序列化结构化输出生成文本。
    std::optional<std::string> text_output = std::nullopt;

    /** @brief 创建成功结果。 @param output 结构化输出。 @return 成功结果。 */
    static ToolResult Success(ToolOutputValue output) { return {Status::Ok(), std::move(output), std::nullopt}; }
    /** @brief 创建失败结果。 @param status 失败状态。 @return 无输出的失败结果。 */
    static ToolResult Failure(Status status) { return {std::move(status), ToolOutputValue::Null(), std::nullopt}; }
};

}  // namespace voicelife
