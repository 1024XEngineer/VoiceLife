#include "linx_json_reader.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace voicelife::linx::detail {
namespace {

void SkipSpaceImpl(std::string_view text, std::size_t& position) {
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
}

bool ReadJsonString(std::string_view text, std::size_t position, std::string& value,
                    std::size_t* next_position = nullptr) {
    SkipSpaceImpl(text, position);
    if (position >= text.size() || text[position] != '"') {
        return false;
    }
    ++position;
    value.clear();
    while (position < text.size()) {
        const char character = text[position++];
        if (character == '"') {
            if (next_position != nullptr) {
                *next_position = position;
            }
            return true;
        }
        if (static_cast<unsigned char>(character) < 0x20U) {
            return false;
        }
        if (character != '\\' || position >= text.size()) {
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            return false;
        }
        const char escaped = text[position++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                if (position + 4 > text.size()) {
                    return false;
                }
                auto hex = [](char digit) -> int {
                    if (digit >= '0' && digit <= '9') return digit - '0';
                    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                    return -1;
                };
                uint32_t code_point = 0;
                for (std::size_t index = 0; index < 4; ++index) {
                    const int digit = hex(text[position + index]);
                    if (digit < 0) return false;
                    code_point = (code_point << 4U) | static_cast<uint32_t>(digit);
                }
                position += 4;
                if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                    if (position + 6 > text.size() || text[position] != '\\' || text[position + 1] != 'u') {
                        return false;
                    }
                    uint32_t low = 0;
                    for (std::size_t index = 0; index < 4; ++index) {
                        const int digit = hex(text[position + 2 + index]);
                        if (digit < 0) return false;
                        low = (low << 4U) | static_cast<uint32_t>(digit);
                    }
                    if (low < 0xDC00U || low > 0xDFFFU) return false;
                    position += 6;
                    code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    return false;
                }
                if (code_point <= 0x7FU) {
                    value.push_back(static_cast<char>(code_point));
                } else if (code_point <= 0x7FFU) {
                    value.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
                    value.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
                } else if (code_point <= 0xFFFFU) {
                    value.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
                    value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                    value.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
                } else {
                    value.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
                    value.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
                    value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                    value.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
                }
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

bool SkipJsonValue(std::string_view object, std::size_t& position) {
    SkipSpaceImpl(object, position);
    if (position >= object.size()) return false;
    if (object[position] == '"') {
        std::string ignored;
        return ReadJsonString(object, position, ignored, &position);
    }
    if (object[position] == '{' || object[position] == '[') {
        const char opening = object[position++];
        const char closing = opening == '{' ? '}' : ']';
        int depth = 1;
        bool in_string = false;
        bool escaped = false;
        while (position < object.size()) {
            const char character = object[position++];
            if (in_string) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    in_string = false;
                continue;
            }
            if (character == '"')
                in_string = true;
            else if (character == opening)
                ++depth;
            else if (character == closing && --depth == 0)
                return true;
        }
        return false;
    }
    const std::size_t begin = position;
    while (position < object.size() && object[position] != ',' && object[position] != '}' && object[position] != ']') {
        ++position;
    }
    return position > begin;
}

bool FindField(std::string_view object, std::string_view key, std::size_t& value_position) {
    std::size_t position = 0;
    SkipSpaceImpl(object, position);
    if (position >= object.size() || object[position] != '{') return false;
    ++position;
    while (position < object.size()) {
        SkipSpaceImpl(object, position);
        if (position < object.size() && object[position] == '}') return false;
        std::string candidate;
        if (!ReadJsonString(object, position, candidate, &position)) return false;
        SkipSpaceImpl(object, position);
        if (position >= object.size() || object[position] != ':') return false;
        value_position = ++position;
        if (candidate == key) return true;
        if (!SkipJsonValue(object, position)) return false;
        SkipSpaceImpl(object, position);
        if (position >= object.size()) return false;
        if (object[position] == ',') {
            ++position;
            continue;
        }
        return false;
    }
    return false;
}

}  // namespace

void SkipSpace(std::string_view text, std::size_t& position) { SkipSpaceImpl(text, position); }

bool ReadStringField(std::string_view object, std::string_view key, std::string& value, bool required,
                     std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少字符串字段: " + std::string(key);
            return false;
        }
        return true;
    }
    if (!ReadJsonString(object, position, value)) {
        error = "字符串字段格式无效: " + std::string(key);
        return false;
    }
    return true;
}

bool ReadUnsignedField(std::string_view object, std::string_view key, uint32_t& value, bool required,
                       std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少数字字段: " + std::string(key);
            return false;
        }
        return true;
    }
    SkipSpaceImpl(object, position);
    const std::size_t begin = position;
    uint64_t parsed = 0;
    while (position < object.size() && std::isdigit(static_cast<unsigned char>(object[position])) != 0) {
        const uint32_t digit = static_cast<uint32_t>(object[position] - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
            error = "数字字段超出范围: " + std::string(key);
            return false;
        }
        parsed = parsed * 10U + digit;
        ++position;
    }
    if (position == begin) {
        error = "数字字段格式无效: " + std::string(key);
        return false;
    }
    SkipSpaceImpl(object, position);
    if (position < object.size() && object[position] != ',' && object[position] != '}') {
        error = "数字字段尾部无效: " + std::string(key);
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ReadBoolField(std::string_view object, std::string_view key, bool& value, bool required, std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少布尔字段: " + std::string(key);
            return false;
        }
        return true;
    }
    SkipSpaceImpl(object, position);
    if (object.substr(position, 4) == "true") {
        value = true;
        position += 4;
        SkipSpaceImpl(object, position);
        if (position < object.size() && object[position] != ',' && object[position] != '}') {
            error = "布尔字段尾部无效: " + std::string(key);
            return false;
        }
        return true;
    }
    if (object.substr(position, 5) == "false") {
        value = false;
        position += 5;
        SkipSpaceImpl(object, position);
        if (position < object.size() && object[position] != ',' && object[position] != '}') {
            error = "布尔字段尾部无效: " + std::string(key);
            return false;
        }
        return true;
    }
    error = "布尔字段格式无效: " + std::string(key);
    return false;
}

bool ReadObjectField(std::string_view object, std::string_view key, std::string_view& value, bool required,
                     std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少对象字段: " + std::string(key);
            return false;
        }
        return true;
    }
    SkipSpaceImpl(object, position);
    if (position >= object.size() || object[position] != '{') {
        error = "对象字段格式无效: " + std::string(key);
        return false;
    }
    const std::size_t begin = position++;
    int depth = 1;
    bool in_string = false;
    bool escaped = false;
    while (position < object.size()) {
        const char character = object[position++];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}' && --depth == 0) {
            value = object.substr(begin, position - begin);
            return true;
        }
    }
    error = "对象字段未闭合: " + std::string(key);
    return false;
}

std::string Quote(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result.push_back('"');
    for (const char character : text) {
        switch (character) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    constexpr char kHex[] = "0123456789abcdef";
                    const unsigned char value = static_cast<unsigned char>(character);
                    result += "\\u00";
                    result.push_back(kHex[value >> 4U]);
                    result.push_back(kHex[value & 0x0FU]);
                } else {
                    result.push_back(character);
                }
                break;
        }
    }
    result.push_back('"');
    return result;
}

}  // namespace voicelife::linx::detail
