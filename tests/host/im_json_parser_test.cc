#include <string>
#include <string_view>

#include "support/test_support.h"
#include "voicelife/contracts/json.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::test::Check;

namespace {

JsonValue ParseOk(std::string_view input) {
    JsonValue value;
    Check(voicelife::ParseJson(input, value).ok(), "JSON 应解析成功");
    return value;
}

void ParseRejected(std::string_view input) {
    JsonValue value;
    const Status status = voicelife::ParseJson(input, value);
    Check(!status.ok() && status.code == ErrorCode::kInvalidArgument, "非法 JSON 应被拒绝");
}

}  // namespace

int main() {
    // 字面量
    Check(ParseOk("null").kind == JsonValue::Kind::kNull, "null 字面量");
    Check(ParseOk("true").boolean && ParseOk("true").kind == JsonValue::Kind::kBool, "true 字面量");
    Check(!ParseOk("false").boolean, "false 字面量");

    // 数字
    Check(ParseOk("0").number == 0.0, "整数 0");
    Check(ParseOk("-1.5e2").number == -150.0, "负指数数字");
    Check(ParseOk("3.25").number == 3.25, "小数");
    Check(ParseOk("2E+3").number == 2000.0, "大写指数");

    // 字符串
    Check(ParseOk("\"\"").string.empty(), "空字符串");
    Check(ParseOk("\"hello\"").string == "hello", "普通字符串");
    Check(ParseOk("\"a\\nb\\tc\"").string == "a\nb\tc", "转义控制字符");
    Check(ParseOk("\"\\u0041\"").string == "A", "unicode 转义");
    Check(ParseOk("\"\\u00e9\"").string == "\xC3\xA9", "unicode 双字节转义");
    Check(ParseOk("\"\\u4e2d\"").string == "中", "unicode 三字节转义");
    Check(ParseOk("\"\\ud83d\\ude00\"").string == "\xF0\x9F\x98\x80", "代理对转义");
    Check(ParseOk("\"知道了\"").string == "知道了", "UTF-8 中文原样保留");
    Check(ParseOk("\"a\\/b\"").string == "a/b", "正斜杠转义");

    // 数组
    Check(ParseOk("[1, 2, 3]").array.size() == 3, "数字数组");
    JsonValue nested = ParseOk("[[1], {\"k\": \"v\"}]");
    Check(nested.array.size() == 2 && nested.array[1].IsObject(), "嵌套数组与对象");
    Check(ParseOk("[]").array.empty(), "空数组");

    // 对象
    JsonValue object = ParseOk("{\"a\": 1, \"b\": [true, null]}");
    Check(object.IsObject() && object.Get("a") != nullptr && object.Get("b")->IsArray(), "对象成员解析");
    Check(ParseOk("{}").object.empty(), "空对象");
    Check(ParseOk("{\"a\":1,\"a\":2}").Get("a")->number == 2.0, "重复键后者覆盖");

    // 空白处理
    Check(ParseOk("  { \"x\" : 1 }  ").Get("x")->number == 1.0, "空白处理");

    // 非法输入拒绝
    ParseRejected("");
    ParseRejected("garbage");
    ParseRejected("{\"a\": 1} trailing");
    ParseRejected("[1, 2");
    ParseRejected("{\"a\" 1}");
    ParseRejected("{\"a\": }");
    ParseRejected("[1,]");
    ParseRejected("\"unterminated");
    ParseRejected("\"bad\\q\"");
    ParseRejected("01");
    ParseRejected("-");
    ParseRejected("1.");
    ParseRejected("1e");
    ParseRejected("tru");
    ParseRejected("nul");
    ParseRejected("{\"a\": 1,}");
    ParseRejected("{\"a\": 1");
    ParseRejected("[1");
    ParseRejected("\"\\ud800\"");
    ParseRejected("\"\\udc00\"");
    ParseRejected("\"\\ud800\\u0041\"");
    ParseRejected("\"\\u12\"");
    // 加固：未转义控制字符 / 深嵌套 / 非法 UTF-8
    ParseRejected("\"a\nb\"");
    ParseRejected(std::string(70, '[') + std::string(70, ']'));
    ParseRejected("\"\xFF\"");
    return 0;
}
