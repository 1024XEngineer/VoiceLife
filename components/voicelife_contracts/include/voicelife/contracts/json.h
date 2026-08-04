#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "voicelife/contracts/status.h"

namespace voicelife {

/// 已序列化的 JSON 文档，具体解析、校验和数据库 JSON 绑定由适配器负责。
struct JsonDocument {
    std::string value;
};

/// JSON 值 DOM。契约解析器消费共享 fixture 前先解析为该结构。
struct JsonValue {
    /// JSON 值种类。
    enum class Kind {
        kNull,
        kBool,
        kNumber,
        kString,
        kArray,
        kObject,
    };

    /// JSON 对象成员表。
    using ObjectMap = std::map<std::string, JsonValue>;

    Kind kind = Kind::kNull;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    /** @brief 构造布尔值节点。 @param value 布尔值。 @return 对应节点。 */
    static JsonValue Bool(bool value);
    /** @brief 构造数值节点。 @param value 数值。 @return 对应节点。 */
    static JsonValue Number(double value);
    /** @brief 构造字符串节点。 @param value 字符串内容。 @return 对应节点。 */
    static JsonValue String(std::string value);
    /** @brief 构造数组节点。 @param value 数组元素。 @return 对应节点。 */
    static JsonValue Array(std::vector<JsonValue> value);
    /** @brief 构造对象节点。 @param value 键值成员。 @return 对应节点。 */
    static JsonValue Object(ObjectMap value);

    /** @brief 判断节点是否为字符串。 @return 是字符串时返回 true。 */
    [[nodiscard]] bool IsString() const { return kind == Kind::kString; }
    /** @brief 判断节点是否为对象。 @return 是对象时返回 true。 */
    [[nodiscard]] bool IsObject() const { return kind == Kind::kObject; }
    /** @brief 判断节点是否为数组。 @return 是数组时返回 true。 */
    [[nodiscard]] bool IsArray() const { return kind == Kind::kArray; }

    /**
     * @brief 读取对象成员。
     * @param key 成员名。
     * @return 非对象或成员缺失时返回 nullptr，否则返回成员指针。
     */
    [[nodiscard]] const JsonValue* Get(const std::string& key) const;
};

/**
 * @brief 把 JSON 文本解析为 DOM。
 * @param input JSON 文本。
 * @param out 解析结果节点。
 * @return 语法或结构非法时返回 kInvalidArgument。
 */
Status ParseJson(std::string_view input, JsonValue& out);

}  // namespace voicelife
