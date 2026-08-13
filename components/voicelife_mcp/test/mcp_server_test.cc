#include "voicelife/mcp/mcp_server.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

#include "support/test_support.h"
#include "yyjson.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::MakeToolOutput;
using voicelife::Status;
using voicelife::ToolResult;
using voicelife::ToolOutputValue;
using voicelife::mcp::McpServer;
using voicelife::mcp::Property;
using voicelife::mcp::PropertyHandler;
using voicelife::mcp::PropertyList;
using voicelife::mcp::PropertyType;
using voicelife::test::Check;

namespace {

/**
 * @brief 注册一个覆盖全部受支持参数类型的测试工具。
 * @param server 待注册工具的 MCP 服务。
 * @param captured_value 接收回调读取到的整数值。
 * @return 工具注册状态。
 */
Status RegisterTypedTool(McpServer& server, int64_t& captured_value) {
    return server.add_tool("self.device.configure", "配置设备",
                           PropertyList({
                               Property("enabled", PropertyType::kBoolean, true).with_description("是否启用"),
                               Property("level", PropertyType::kInteger, 0, 100).with_description("等级"),
                               Property("label", PropertyType::kString, 1, 10, std::string("default"))
                                   .with_description("标签"),
                           }),
                           [&captured_value](const PropertyList& properties) {
                               captured_value = properties.value<int64_t>("level").value_or(-1);
                               return ToolResult::Success(ToolOutputValue::Null());
                           });
}

/**
 * @brief 验证参数列表生成 Schema 并安全读取绑定值。
 * @return 无。
 */
void TestPropertyList() {
    PropertyList properties;
    properties.add_property(Property("enabled", PropertyType::kBoolean, true).with_description("是否启用"));
    properties.add_property(Property("level", PropertyType::kInteger, 0, 100).with_description("等级"));

    const auto schema = properties.to_schema();
    Check(schema.properties.size() == 2 && schema.required.size() == 1 && schema.required.front() == "level" &&
              schema.properties.at("enabled").description == "是否启用" &&
              schema.properties.at("level").description == "等级",
          "无默认值的参数应标记为必填");

    const auto values = properties.with_values({{"enabled", false}, {"level", int64_t{25}}});
    Check(values.value<bool>("enabled") == false, "应读取匹配类型的参数值");
    Check(!values.value<std::string>("enabled").has_value(), "参数类型不匹配时应返回空值");
    Check(!values.value<int64_t>("missing").has_value(), "参数不存在时应返回空值");

    PropertyList optional;
    optional.add_property(Property::Optional("location", PropertyType::kString));
    const auto optional_schema = optional.to_schema();
    Check(optional_schema.required.empty() && optional_schema.properties.contains("location"),
          "无默认值的可选参数不应进入 required");

    PropertyList object;
    object.add_property(Property::OptionalObject(
        "settings",
        PropertyList({
            Property("brightness", PropertyType::kInteger, 0, 100).with_description("亮度"),
            Property::Optional("label", PropertyType::kString).with_description("标签"),
        }))
                            .with_description("配置对象"));
    const auto object_schema = object.to_schema();
    Check(object_schema.required.empty() && object_schema.properties.contains("settings") &&
              object_schema.properties.at("settings").type == voicelife::mcp::ToolInputType::kObject,
          "对象参数应标记为 object 类型且可省略");
    const auto settings_schema = object_schema.properties.at("settings").object_schema;
    Check(settings_schema != nullptr && settings_schema->properties.contains("brightness") &&
              settings_schema->properties.at("brightness").type == voicelife::mcp::ToolInputType::kInteger &&
              settings_schema->required.size() == 1 && settings_schema->required.front() == "brightness",
          "对象参数应能递归生成内部字段 Schema");

    const auto object_values = object.with_values(
        {{"settings", JsonValue::Object({{"brightness", JsonValue::Number(80)}})}});
    const auto settings = object_values.value<JsonValue>("settings");
    Check(settings.has_value() && settings->IsObject() && settings->Get("brightness") != nullptr &&
              settings->Get("brightness")->number == 80,
          "对象参数应可通过 JsonValue 读取");
}

/**
 * @brief 验证工具注册拒绝不完整或无效的参数定义。
 * @return 无。
 */
void TestRegistrationValidation() {
    McpServer server;
    const PropertyHandler handler = [](const PropertyList&) {
        return ToolResult::Success(ToolOutputValue::Null());
    };

    Check(server.add_tool("", "描述", {}, handler).code == ErrorCode::kInvalidArgument, "工具名称为空时应拒绝注册");
    Check(server.add_tool("invalid.description", "", {}, handler).code == ErrorCode::kInvalidArgument,
          "工具描述为空时应拒绝注册");
    Check(server.add_tool("invalid.handler", "描述", {}, {}).code == ErrorCode::kInvalidArgument,
          "工具回调为空时应拒绝注册");
    Check(server.add_tool("invalid.default", "描述",
                          PropertyList({Property("enabled", PropertyType::kBoolean, std::string("true"))}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "默认值类型错误时应拒绝注册");
    Check(server.add_tool("invalid.object_default", "描述",
                          PropertyList({Property("settings", PropertyType::kObject, std::string("{}"))}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "对象参数默认值类型错误时应拒绝注册");
    Check(server.add_tool("invalid.boolean_range", "描述",
                          PropertyList({Property("enabled", PropertyType::kBoolean, 0, 1)}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "布尔参数声明范围时应拒绝注册");
    Check(server.add_tool("invalid.range", "描述", PropertyList({Property("level", PropertyType::kInteger, 10, 0)}),
                          handler)
                  .code == ErrorCode::kInvalidArgument,
          "整数参数最小值大于最大值时应拒绝注册");
    Check(server.add_tool("invalid.integer_default", "描述",
                          PropertyList({Property("level", PropertyType::kInteger, 0, 100, int64_t{101})}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "整数默认值超出范围时应拒绝注册");
    Check(server.add_tool("invalid.integer_default_low", "描述",
                          PropertyList({Property("level", PropertyType::kInteger, 0, 100, int64_t{-1})}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "整数默认值低于范围时应拒绝注册");
    Check(server.add_tool("invalid.string_length", "描述",
                          PropertyList({Property("label", PropertyType::kString, 10, 1)}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "字符串最小长度大于最大长度时应拒绝注册");
    Check(server.add_tool("invalid.string_default", "描述",
                          PropertyList({Property("label", PropertyType::kString, 1, 3, std::string("默认值过长"))}),
                          handler)
                  .code == ErrorCode::kInvalidArgument,
          "字符串默认值超出长度范围时应拒绝注册");
    Check(server.add_tool("invalid.string_negative_length", "描述",
                          PropertyList({Property("label", PropertyType::kString, -1, 3)}), handler)
                  .code == ErrorCode::kInvalidArgument,
          "字符串长度不能为负数");
}

/**
 * @brief 验证工具调用的成功路径、默认值和参数错误处理。
 * @return 无。
 */
void TestToolCalls() {
    McpServer server;
    int64_t captured_value = 0;
    Check(RegisterTypedTool(server, captured_value).ok(), "合法工具应注册成功");
    Check(RegisterTypedTool(server, captured_value).code == ErrorCode::kAlreadyExists, "同名工具不能重复注册");

    const auto success = server.call({
        .request_id = "request-1",
        .name = "self.device.configure",
        .arguments = {{"enabled", false}, {"level", int64_t{42}}, {"label", std::string("desk")}},
    });
    Check(success.status.ok() && captured_value == 42, "布尔、整数和字符串参数应通过校验并进入回调");

    const auto defaults = server.call({
        .request_id = "request-2",
        .name = "self.device.configure",
        .arguments = {{"level", int64_t{20}}},
    });
    Check(defaults.status.ok(), "可选参数缺失时应使用默认值");

    Check(server.call({.request_id = {}, .name = "self.device.configure", .arguments = {}}).status.code ==
              ErrorCode::kInvalidArgument,
          "请求 ID 为空时应拒绝调用");
    Check(server.call({.request_id = "request-3", .name = "unknown", .arguments = {}}).status.code ==
              ErrorCode::kNotFound,
          "工具不存在时应返回未找到");
    Check(server.call({.request_id = "request-4", .name = "self.device.configure", .arguments = {}}).status.code ==
              ErrorCode::kInvalidArgument,
          "缺少必填参数时应拒绝调用");
    Check(server.call({
                          .request_id = "request-5",
                          .name = "self.device.configure",
                          .arguments = {{"level", std::string("42")}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "参数类型不匹配时应拒绝调用");
    Check(server.call({
                          .request_id = "request-6",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{-1}}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "整数参数低于下限时应拒绝调用");
    Check(server.call({
                          .request_id = "request-7",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{101}}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "整数参数高于上限时应拒绝调用");
    Check(server.call({
                          .request_id = "request-8",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{10}}, {"unknown", true}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "未定义参数应被拒绝");

    Check(server
              .add_tool("self.device.optional", "可选参数测试",
                        PropertyList({Property::Optional("location", PropertyType::kString)}),
                        [](const PropertyList& properties) {
                            return ToolResult::Success(ToolOutputValue::Object({
                                MakeToolOutput("location",
                                               ToolOutputValue::String(properties.value<std::string>("location").value_or("none"))),
                            }));
                        })
              .ok(),
          "无默认值的可选参数应能注册");
    Check(server.call({.request_id = "request-optional", .name = "self.device.optional", .arguments = {}}).status.ok(),
          "省略可选参数时调用应通过");
    Check(server.call({
                          .request_id = "request-9",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{10}}, {"label", std::string("01234567890")}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "字符串参数超过长度上限时应拒绝调用");
    Check(server.call({
                          .request_id = "request-10",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{10}}, {"label", std::string()}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "字符串参数低于长度下限时应拒绝调用");
    Check(server
              .call({
                  .request_id = "request-11",
                  .name = "self.device.configure",
                  .arguments = {{"level", int64_t{10}}, {"label", std::string("中文标签")}},
              })
              .status.ok(),
          "UTF-8 字符串长度应按字符数校验");

    Check(server
              .add_tool("self.device.object", "对象参数测试",
                        PropertyList({Property::OptionalObject(
                            "settings",
                            PropertyList({
                                Property("brightness", PropertyType::kInteger, 0, 100).with_description("亮度"),
                                Property("enabled", PropertyType::kBoolean, true),
                                Property::OptionalObject(
                                    "network",
                                    PropertyList({
                                        Property("mode", PropertyType::kString).with_description("模式"),
                                        Property::Optional("retry", PropertyType::kInteger),
                                    })),
                            }))}),
                        [](const PropertyList& properties) {
                            const auto settings = properties.value<JsonValue>("settings");
                            const JsonValue* network = settings.has_value() ? settings->Get("network") : nullptr;
                            return ToolResult::Success(ToolOutputValue::Object({
                                MakeToolOutput("has_brightness",
                                               ToolOutputValue::Boolean(settings.has_value() && settings->IsObject() &&
                                                                         settings->Get("brightness") != nullptr)),
                                MakeToolOutput("has_network",
                                               ToolOutputValue::Boolean(network != nullptr && network->IsObject())),
                            }));
                        })
              .ok(),
          "对象参数应能注册");
    Check(server
              .call({
                  .request_id = "request-object",
                  .name = "self.device.object",
                  .arguments = {{"settings",
                                 JsonValue::Object({
                                     {"brightness", JsonValue::Number(64)},
                                     {"network", JsonValue::Object({{"mode", JsonValue::String("wifi")}})},
                                 })}},
              })
              .status.ok(),
          "对象参数应通过校验并进入回调");
    Check(server
              .call({
                  .request_id = "request-object-missing",
                  .name = "self.device.object",
                  .arguments = {{"settings", JsonValue::Object({{"enabled", JsonValue::Bool(true)}})}},
              })
              .status.code == ErrorCode::kInvalidArgument,
          "对象参数缺少内部必填字段时应拒绝调用");
    Check(server
              .call({
                  .request_id = "request-object-unknown",
                  .name = "self.device.object",
                  .arguments = {{"settings",
                                 JsonValue::Object({{"brightness", JsonValue::Number(1)},
                                                    {"unknown", JsonValue::Bool(true)}})}},
              })
              .status.code == ErrorCode::kInvalidArgument,
          "对象参数包含未定义字段时应拒绝调用");
    Check(server
              .call({
                  .request_id = "request-object-invalid",
                  .name = "self.device.object",
                  .arguments = {{"settings", std::string("not-an-object")}},
              })
              .status.code == ErrorCode::kInvalidArgument,
          "对象参数传入字符串时应拒绝调用");
}

/**
 * @brief 验证工具列表和 JSON Schema 序列化结果。
 * @return 无。
 */
void TestToolListing() {
    McpServer server;
    int64_t captured_value = 0;
    Check(server.list_tools().total == 0, "MCP 服务初始不应包含工具");
    const std::string empty_json = server.list_tools_json();
    yyjson_doc* empty_document = yyjson_read(empty_json.data(), empty_json.size(), YYJSON_READ_NOFLAG);
    Check(empty_document != nullptr, "空工具列表应序列化为合法 JSON");
    yyjson_val* empty_tools = yyjson_obj_get(yyjson_doc_get_root(empty_document), "tools");
    Check(yyjson_is_arr(empty_tools) && yyjson_arr_size(empty_tools) == 0, "空工具列表应包含空 tools 数组");
    yyjson_doc_free(empty_document);

    Check(RegisterTypedTool(server, captured_value).ok(), "列表测试工具应注册成功");
    const PropertyHandler handler = [](const PropertyList&) {
        return ToolResult::Success(ToolOutputValue::Null());
    };
    Check(server
              .add_tool("self.device.boundary", "整数范围\"测试\\路径",
                        PropertyList({Property("value", PropertyType::kInteger, std::numeric_limits<int64_t>::min(),
                                               std::numeric_limits<int64_t>::max())}),
                        handler)
              .ok(),
          "整数边界工具应注册成功");
    Check(server
              .add_tool("self.device.object", "对象参数测试",
                        PropertyList({Property(
                            "settings",
                            PropertyList({
                                Property("brightness", PropertyType::kInteger, 0, 100).with_description("亮度"),
                                Property::OptionalObject(
                                    "network",
                                    PropertyList({
                                        Property("mode", PropertyType::kString).with_description("模式"),
                                    })),
                            }))}),
                        handler)
              .ok(),
          "对象参数工具应注册成功");

    const auto listed = server.list_tools();
    Check(listed.total == 3 && listed.tools.front().name == "self.device.configure", "工具列表应返回注册结果");
    const std::string json = server.list_tools_json();
    std::cout << json << '\n';
    yyjson_doc* document = yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG);
    Check(document != nullptr, "tools/list 应序列化为合法 JSON");
    yyjson_val* tools = yyjson_obj_get(yyjson_doc_get_root(document), "tools");
    Check(yyjson_is_arr(tools) && yyjson_arr_size(tools) == 3, "tools/list JSON 应包含全部工具");

    yyjson_val* configure = yyjson_arr_get(tools, 0);
    yyjson_val* configure_schema = yyjson_obj_get(configure, "inputSchema");
    yyjson_val* properties = yyjson_obj_get(configure_schema, "properties");
    yyjson_val* enabled = yyjson_obj_get(properties, "enabled");
    yyjson_val* level = yyjson_obj_get(properties, "level");
    yyjson_val* label = yyjson_obj_get(properties, "label");
    Check(yyjson_equals_str(yyjson_obj_get(configure, "name"), "self.device.configure") &&
              yyjson_equals_str(yyjson_obj_get(level, "type"), "integer") &&
              yyjson_get_sint(yyjson_obj_get(level, "minimum")) == 0 &&
              yyjson_get_sint(yyjson_obj_get(level, "maximum")) == 100,
          "tools/list JSON 应包含完整的工具输入 Schema");
    Check(yyjson_equals_str(yyjson_obj_get(enabled, "description"), "是否启用") &&
              yyjson_equals_str(yyjson_obj_get(level, "description"), "等级") &&
              yyjson_equals_str(yyjson_obj_get(label, "description"), "标签"),
          "字段描述应序列化到 JSON Schema");
    Check(yyjson_get_sint(yyjson_obj_get(label, "minLength")) == 1 &&
              yyjson_get_sint(yyjson_obj_get(label, "maxLength")) == 10,
          "字符串长度范围应序列化到 Schema");

    yyjson_val* boundary = yyjson_arr_get(tools, 1);
    yyjson_val* boundary_schema = yyjson_obj_get(boundary, "inputSchema");
    yyjson_val* boundary_properties = yyjson_obj_get(boundary_schema, "properties");
    yyjson_val* value = yyjson_obj_get(boundary_properties, "value");
    Check(yyjson_equals_str(yyjson_obj_get(boundary, "description"), "整数范围\"测试\\路径") &&
              yyjson_is_sint(yyjson_obj_get(value, "minimum")) &&
              yyjson_get_sint(yyjson_obj_get(value, "minimum")) == std::numeric_limits<int64_t>::min() &&
              yyjson_is_int(yyjson_obj_get(value, "maximum")) &&
              yyjson_get_sint(yyjson_obj_get(value, "maximum")) == std::numeric_limits<int64_t>::max(),
          "tools/list JSON 应保留特殊字符和完整 int64_t 边界");

    yyjson_val* object = yyjson_arr_get(tools, 2);
    yyjson_val* object_schema = yyjson_obj_get(object, "inputSchema");
    yyjson_val* object_properties = yyjson_obj_get(object_schema, "properties");
    yyjson_val* settings = yyjson_obj_get(object_properties, "settings");
    Check(yyjson_equals_str(yyjson_obj_get(settings, "type"), "object"),
          "对象参数的 JSON Schema 类型应为 object");
    yyjson_val* settings_properties = yyjson_obj_get(settings, "properties");
    yyjson_val* brightness = yyjson_obj_get(settings_properties, "brightness");
    yyjson_val* network = yyjson_obj_get(settings_properties, "network");
    yyjson_val* network_properties = yyjson_obj_get(network, "properties");
    Check(yyjson_equals_str(yyjson_obj_get(brightness, "type"), "integer") &&
              yyjson_equals_str(yyjson_obj_get(network, "type"), "object") &&
              yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(network_properties, "mode"), "type"), "string"),
          "对象参数的内部字段应递归序列化到 JSON Schema");
    yyjson_doc_free(document);
}

}  // namespace

/**
 * @brief 验证 MCP 工具注册、参数校验、调用和列表序列化能力。
 * @return 全部断言通过时返回 0。
 */
int main() {
    TestPropertyList();
    TestRegistrationValidation();
    TestToolCalls();
    TestToolListing();
    return 0;
}
