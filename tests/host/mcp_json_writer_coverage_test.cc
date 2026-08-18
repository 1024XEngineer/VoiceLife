#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "support/test_support.h"
#include "voicelife/contracts/tool.h"
#include "voicelife/mcp/mcp_server.h"
#include "yyjson.h"

using voicelife::MakeToolOutput;
using voicelife::ToolOutputArray;
using voicelife::ToolOutputObject;
using voicelife::ToolOutputValue;
using voicelife::mcp::SerializeToolOutputValue;
using voicelife::test::Check;

namespace {

/** @brief 构造用于覆盖数组序列化空指针元素的工具输出数组。 @return 测试数组。 */
ToolOutputArray BuildArrayWithNull() {
    return {
        MakeToolOutput(ToolOutputValue::Null()),
        MakeToolOutput(ToolOutputValue::Boolean(true)),
        MakeToolOutput(ToolOutputValue::Integer(42)),
        MakeToolOutput(ToolOutputValue::String("text")),
        nullptr,
    };
}

/** @brief 构造用于覆盖对象序列化空指针成员的工具输出对象。 @param array 测试数组。 @return 测试对象。 */
ToolOutputObject BuildObjectWithNull(ToolOutputArray array) {
    return {
        MakeToolOutput("ok", ToolOutputValue::Boolean(false)),
        MakeToolOutput("count", ToolOutputValue::Integer(7)),
        MakeToolOutput("items", ToolOutputValue::Array(std::move(array))),
        {"missing", nullptr},
    };
}

}  // namespace

/**
 * @brief 执行新增的 MCP JSON 输出序列化覆盖测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    const std::string json =
        SerializeToolOutputValue(ToolOutputValue::Object(BuildObjectWithNull(BuildArrayWithNull())));
    yyjson_doc* document = yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG);
    Check(document != nullptr, "工具输出对象应序列化为合法 JSON");

    yyjson_val* root = yyjson_doc_get_root(document);
    Check(yyjson_is_obj(root), "工具输出对象根节点应为对象");
    Check(yyjson_is_false(yyjson_obj_get(root, "ok")) && yyjson_is_null(yyjson_obj_get(root, "missing")),
          "对象空指针成员应序列化为 null");

    yyjson_val* items = yyjson_obj_get(root, "items");
    Check(yyjson_is_arr(items) && yyjson_arr_size(items) == 5, "数组空指针元素应序列化为 null");
    Check(yyjson_is_null(yyjson_arr_get(items, 0)) && yyjson_is_true(yyjson_arr_get(items, 1)) &&
              yyjson_get_sint(yyjson_arr_get(items, 2)) == 42 && yyjson_is_null(yyjson_arr_get(items, 4)),
          "数组标量与空指针序列化结果应正确");
    yyjson_doc_free(document);
    return 0;
}
