#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace voicelife::linx::detail {

/** @brief 跳过字符串中的空白字符。 */
void SkipSpace(std::string_view text, std::size_t& position);

/** @brief 读取必填或可选字符串字段。 */
bool ReadStringField(std::string_view object, std::string_view key, std::string& value, bool required,
                     std::string& error);

/** @brief 读取必填或可选无符号数字字段。 */
bool ReadUnsignedField(std::string_view object, std::string_view key, uint32_t& value, bool required,
                       std::string& error);

/** @brief 读取必填或可选布尔字段。 */
bool ReadBoolField(std::string_view object, std::string_view key, bool& value, bool required, std::string& error);

/** @brief 读取必填或可选 JSON 对象字段。 */
bool ReadObjectField(std::string_view object, std::string_view key, std::string_view& value, bool required,
                     std::string& error);

/** @brief 将字符串编码为带转义的 JSON 字符串字面量。 */
std::string Quote(std::string_view text);

}  // namespace voicelife::linx::detail
