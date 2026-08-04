#include "voicelife/contracts/json.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace voicelife {

JsonValue JsonValue::Bool(bool value) {
    JsonValue result;
    result.kind = Kind::kBool;
    result.boolean = value;
    return result;
}

JsonValue JsonValue::Number(double value) {
    JsonValue result;
    result.kind = Kind::kNumber;
    result.number = value;
    return result;
}

JsonValue JsonValue::String(std::string value) {
    JsonValue result;
    result.kind = Kind::kString;
    result.string = std::move(value);
    return result;
}

JsonValue JsonValue::Array(std::vector<JsonValue> value) {
    JsonValue result;
    result.kind = Kind::kArray;
    result.array = std::move(value);
    return result;
}

JsonValue JsonValue::Object(std::map<std::string, JsonValue> value) {
    JsonValue result;
    result.kind = Kind::kObject;
    result.object = std::move(value);
    return result;
}

const JsonValue* JsonValue::Get(const std::string& key) const {
    if (!IsObject()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

namespace {

class JsonParser {
   public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    Status Parse(JsonValue& out) {
        SkipWhitespace();
        const Status value_status = ParseValue(out);
        if (!value_status.ok()) {
            return value_status;
        }
        SkipWhitespace();
        if (position_ != input_.size()) {
            return Status::Error(ErrorCode::kInvalidArgument, "JSON 文档尾部存在多余内容");
        }
        return Status::Ok();
    }

   private:
    [[nodiscard]] Status Error(const char* message) const {
        return Status::Error(ErrorCode::kInvalidArgument, message);
    }

    void SkipWhitespace() {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    bool IsDigitAt(size_t offset) const {
        return position_ + offset < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_ + offset])) != 0;
    }

    Status ParseValue(JsonValue& out) {
        SkipWhitespace();
        if (position_ >= input_.size()) {
            return Error("JSON 值缺失");
        }
        switch (input_[position_]) {
            case '{':
                return ParseObject(out);
            case '[':
                return ParseArray(out);
            case '"':
                return ParseString(out);
            case 't':
                return ParseLiteral("true", out);
            case 'f':
                return ParseLiteral("false", out);
            case 'n':
                return ParseLiteral("null", out);
            default:
                if (input_[position_] == '-' || IsDigitAt(0)) {
                    return ParseNumber(out);
                }
                return Error("未知 JSON 值起始字符");
        }
    }

    Status ParseLiteral(const char* literal, JsonValue& out) {
        const std::string_view text(literal);
        if (input_.substr(position_, text.size()) != text) {
            return Error("非法 JSON 字面量");
        }
        position_ += text.size();
        if (text == "null") {
            out.kind = JsonValue::Kind::kNull;
        } else {
            out = JsonValue::Bool(text == "true");
        }
        return Status::Ok();
    }

    Status ParseNumber(JsonValue& out) {
        const size_t start = position_;
        Consume('-');
        if (Consume('0')) {
            // 前导零之后不允许紧跟数字。
            if (IsDigitAt(0)) {
                return Error("JSON 数字前导零非法");
            }
        } else if (IsDigitAt(0)) {
            while (IsDigitAt(0)) {
                ++position_;
            }
        } else {
            return Error("JSON 数字缺少整数部分");
        }
        if (Consume('.')) {
            if (!IsDigitAt(0)) {
                return Error("JSON 数字缺少小数部分");
            }
            while (IsDigitAt(0)) {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (!IsDigitAt(0)) {
                return Error("JSON 数字缺少指数部分");
            }
            while (IsDigitAt(0)) {
                ++position_;
            }
        }
        const std::string token(input_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(value)) {
            return Error("JSON 数字超出可表示范围");
        }
        out = JsonValue::Number(value);
        return Status::Ok();
    }

    Status ParseString(JsonValue& out) {
        ++position_;  // 跳过起始引号。
        std::string result;
        while (position_ < input_.size()) {
            const char current = input_[position_];
            if (current == '"') {
                ++position_;
                out = JsonValue::String(std::move(result));
                return Status::Ok();
            }
            if (current != '\\') {
                result.push_back(current);
                ++position_;
                continue;
            }
            ++position_;  // 跳过反斜杠。
            if (position_ >= input_.size()) {
                return Error("字符串转义不完整");
            }
            switch (input_[position_]) {
                case '"':
                    result.push_back('"');
                    ++position_;
                    break;
                case '\\':
                    result.push_back('\\');
                    ++position_;
                    break;
                case '/':
                    result.push_back('/');
                    ++position_;
                    break;
                case 'b':
                    result.push_back('\b');
                    ++position_;
                    break;
                case 'f':
                    result.push_back('\f');
                    ++position_;
                    break;
                case 'n':
                    result.push_back('\n');
                    ++position_;
                    break;
                case 'r':
                    result.push_back('\r');
                    ++position_;
                    break;
                case 't':
                    result.push_back('\t');
                    ++position_;
                    break;
                case 'u': {
                    ++position_;
                    unsigned int code_unit = 0;
                    if (!TryHexValue(4, code_unit)) {
                        return Error("\\u 转义缺少 4 位十六进制");
                    }
                    if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
                        if (position_ + 1 < input_.size() && input_[position_] == '\\' &&
                            input_[position_ + 1] == 'u') {
                            position_ += 2;
                            unsigned int low = 0;
                            if (!TryHexValue(4, low) || low < 0xDC00 || low > 0xDFFF) {
                                return Error("代理对低位无效");
                            }
                            AppendUtf8(result, 0x10000 + ((code_unit - 0xD800) << 10) + (low - 0xDC00));
                        } else {
                            return Error("高代理对缺少低位");
                        }
                    } else if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
                        return Error("出现未配对低位代理");
                    } else {
                        AppendUtf8(result, code_unit);
                    }
                    break;
                }
                default:
                    return Error("未知字符串转义");
            }
        }
        return Error("字符串缺少结束引号");
    }

    bool TryHexValue(size_t count, unsigned int& out) {
        unsigned int value = 0;
        for (size_t i = 0; i < count; ++i) {
            if (position_ >= input_.size()) {
                return false;
            }
            const int digit = HexDigit(input_[position_]);
            if (digit < 0) {
                return false;
            }
            value = value * 16 + static_cast<unsigned int>(digit);
            ++position_;
        }
        out = value;
        return true;
    }

    Status ParseArray(JsonValue& out) {
        ++position_;  // 跳过左方括号。
        std::vector<JsonValue> items;
        SkipWhitespace();
        if (Consume(']')) {
            out = JsonValue::Array(std::move(items));
            return Status::Ok();
        }
        while (true) {
            JsonValue item;
            const Status item_status = ParseValue(item);
            if (!item_status.ok()) {
                return item_status;
            }
            items.push_back(std::move(item));
            SkipWhitespace();
            if (Consume(']')) {
                break;
            }
            if (!Consume(',')) {
                return Error("JSON 数组缺少逗号或结束符");
            }
        }
        out = JsonValue::Array(std::move(items));
        return Status::Ok();
    }

    Status ParseObject(JsonValue& out) {
        ++position_;  // 跳过左花括号。
        std::map<std::string, JsonValue> members;
        SkipWhitespace();
        if (Consume('}')) {
            out = JsonValue::Object(std::move(members));
            return Status::Ok();
        }
        while (true) {
            SkipWhitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                return Error("JSON 对象键必须是字符串");
            }
            JsonValue key;
            const Status key_status = ParseString(key);
            if (!key_status.ok()) {
                return key_status;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return Error("JSON 对象键后缺少冒号");
            }
            JsonValue value;
            const Status value_status = ParseValue(value);
            if (!value_status.ok()) {
                return value_status;
            }
            members[std::move(key.string)] = std::move(value);
            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            if (!Consume(',')) {
                return Error("JSON 对象缺少逗号或结束符");
            }
        }
        out = JsonValue::Object(std::move(members));
        return Status::Ok();
    }

    static int HexDigit(char value) {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    static void AppendUtf8(std::string& out, unsigned int codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    std::string_view input_;
    size_t position_ = 0;
};

}  // namespace

Status ParseJson(std::string_view input, JsonValue& out) { return JsonParser(input).Parse(out); }

}  // namespace voicelife
