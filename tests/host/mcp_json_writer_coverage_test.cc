#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "mcp_json_writer.h"
#include "support/test_support.h"
#include "voicelife/contracts/tool.h"
#include "voicelife/mcp/mcp_server.h"
#include "yyjson.h"

using voicelife::MakeToolOutput;
using voicelife::ToolOutputArray;
using voicelife::ToolOutputObject;
using voicelife::ToolOutputValue;
using voicelife::mcp::ListToolsResult;
using voicelife::mcp::SerializeListToolsResult;
using voicelife::mcp::SerializeToolOutputValue;
using voicelife::mcp::ToolDefinition;
using voicelife::mcp::ToolInputField;
using voicelife::mcp::ToolInputSchema;
using voicelife::mcp::ToolInputType;
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

ToolInputField MakeField(ToolInputType type) {
    ToolInputField field{};
    field.type = type;
    return field;
}

void CheckListToolsSchemaSerialization() {
    auto nested = std::make_shared<ToolInputSchema>();
    ToolInputField nested_name = MakeField(ToolInputType::kString);
    nested_name.description = "嵌套名称";
    nested_name.min_length = 1;
    nested_name.max_length = 8;
    nested->properties.emplace("name", std::move(nested_name));
    nested->properties.emplace("enabled", MakeField(ToolInputType::kBoolean));
    nested->required.push_back("name");

    ToolInputSchema schema;
    schema.properties.emplace("flag", MakeField(ToolInputType::kBoolean));
    ToolInputField count_field = MakeField(ToolInputType::kInteger);
    count_field.description = "数量";
    count_field.minimum = -2;
    count_field.maximum = 9;
    schema.properties.emplace("count", std::move(count_field));
    ToolInputField label_field = MakeField(ToolInputType::kString);
    label_field.description = "标签";
    label_field.min_length = 2;
    label_field.max_length = 12;
    schema.properties.emplace("label", std::move(label_field));
    ToolInputField settings_field = MakeField(ToolInputType::kObject);
    settings_field.description = "设置";
    settings_field.object_schema = nested;
    schema.properties.emplace("settings", std::move(settings_field));
    schema.required = {"flag", "settings"};

    const std::string json = SerializeListToolsResult(ListToolsResult{
        .tools = {ToolDefinition{.name = "coverage.schema", .description = "覆盖 Schema", .input_schema = schema}},
        .total = 1,
    });
    yyjson_doc* document = yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG);
    Check(document != nullptr, "工具列表 Schema 应序列化为合法 JSON");
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* tools = yyjson_obj_get(root, "tools");
    Check(yyjson_is_arr(tools) && yyjson_arr_size(tools) == 1, "工具列表应包含定义");
    yyjson_val* tool = yyjson_arr_get(tools, 0);
    Check(yyjson_equals_str(yyjson_obj_get(tool, "name"), "coverage.schema") &&
              yyjson_equals_str(yyjson_obj_get(tool, "description"), "覆盖 Schema"),
          "工具名称和描述应序列化");

    yyjson_val* input_schema = yyjson_obj_get(tool, "inputSchema");
    yyjson_val* properties = yyjson_obj_get(input_schema, "properties");
    yyjson_val* count = yyjson_obj_get(properties, "count");
    yyjson_val* label = yyjson_obj_get(properties, "label");
    Check(yyjson_equals_str(yyjson_obj_get(input_schema, "type"), "object") &&
              yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(properties, "flag"), "type"), "boolean") &&
              yyjson_equals_str(yyjson_obj_get(count, "type"), "integer") &&
              yyjson_get_sint(yyjson_obj_get(count, "minimum")) == -2 &&
              yyjson_get_sint(yyjson_obj_get(count, "maximum")) == 9 &&
              yyjson_get_sint(yyjson_obj_get(label, "minLength")) == 2 &&
              yyjson_get_sint(yyjson_obj_get(label, "maxLength")) == 12,
          "标量字段的类型、描述约束应序列化");

    yyjson_val* settings = yyjson_obj_get(properties, "settings");
    yyjson_val* nested_properties = yyjson_obj_get(settings, "properties");
    yyjson_val* required = yyjson_obj_get(settings, "required");
    yyjson_val* schema_required = yyjson_obj_get(input_schema, "required");
    Check(yyjson_equals_str(yyjson_obj_get(settings, "type"), "object") &&
              yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(nested_properties, "name"), "description"), "嵌套名称") &&
              yyjson_arr_size(required) == 1 && yyjson_equals_str(yyjson_arr_get(required, 0), "name") &&
              yyjson_arr_size(schema_required) == 2,
          "嵌套对象属性和 required 数组应序列化");
    yyjson_doc_free(document);
}

/**
 * @brief 执行新增的 MCP JSON 输出序列化覆盖测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    CheckListToolsSchemaSerialization();
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
    // 空容器和分页边界也必须产生稳定的 JSON，而不是依赖实现细节。
    Check(SerializeToolOutputValue(ToolOutputValue::Array({})) == "[]", "空数组应序列化为 []");
    Check(SerializeToolOutputValue(ToolOutputValue::Object({})) == "{}", "空对象应序列化为 {}");
    const ListToolsResult empty_list{.tools = {}, .total = 0};
    Check(SerializeListToolsResult(empty_list) == R"({"tools":[],"nextCursor":null})", "空工具列表应带终止游标");
    Check(voicelife::mcp::SerializeListToolsResultPage(empty_list, 1, 0) == "{}", "反向分页范围应返回空对象");
    Check(voicelife::mcp::SerializeListToolsResultPage(empty_list, 0, 1) == "{}", "越界分页范围应返回空对象");
    return 0;
}
