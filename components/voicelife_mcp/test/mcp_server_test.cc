#include "voicelife/mcp/mcp_server.h"

#include <iostream>
#include <string>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolResult;
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
                           PropertyList({Property("enabled", PropertyType::kBoolean, true),
                                         Property("level", PropertyType::kInteger, 0, 100),
                                         Property("label", PropertyType::kString, std::string("default"))}),
                           [&captured_value](const PropertyList& properties) {
                               captured_value = properties.value<int64_t>("level").value_or(-1);
                               return ToolResult{.status = Status::Ok(), .output = {}};
                           });
}

/**
 * @brief 验证参数列表生成 Schema 并安全读取绑定值。
 * @return 无。
 */
void TestPropertyList() {
    PropertyList properties;
    properties.add_property(Property("enabled", PropertyType::kBoolean, true));
    properties.add_property(Property("level", PropertyType::kInteger, 0, 100));

    const auto schema = properties.to_schema();
    Check(schema.properties.size() == 2 && schema.required.size() == 1 && schema.required.front() == "level",
          "无默认值的参数应标记为必填");

    const auto values = properties.with_values({{"enabled", false}, {"level", int64_t{25}}});
    Check(values.value<bool>("enabled") == false, "应读取匹配类型的参数值");
    Check(!values.value<std::string>("enabled").has_value(), "参数类型不匹配时应返回空值");
    Check(!values.value<int64_t>("missing").has_value(), "参数不存在时应返回空值");
}

/**
 * @brief 验证工具注册拒绝不完整或无效的参数定义。
 * @return 无。
 */
void TestRegistrationValidation() {
    McpServer server;
    const PropertyHandler handler = [](const PropertyList&) {
        return ToolResult{.status = Status::Ok(), .output = {}};
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
    Check(server.add_tool("invalid.type_range", "描述", PropertyList({Property("label", PropertyType::kString, 0, 10)}),
                          handler)
                  .code == ErrorCode::kInvalidArgument,
          "非整数参数声明范围时应拒绝注册");
    Check(server.add_tool("invalid.range", "描述", PropertyList({Property("level", PropertyType::kInteger, 10, 0)}),
                          handler)
                  .code == ErrorCode::kInvalidArgument,
          "整数参数最小值大于最大值时应拒绝注册");
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
}

/**
 * @brief 验证工具列表和 JSON Schema 序列化结果。
 * @return 无。
 */
void TestToolListing() {
    McpServer server;
    int64_t captured_value = 0;
    Check(server.list_tools().total == 0, "MCP 服务初始不应包含工具");
    Check(RegisterTypedTool(server, captured_value).ok(), "列表测试工具应注册成功");

    const auto listed = server.list_tools();
    Check(listed.total == 1 && listed.tools.front().name == "self.device.configure", "工具列表应返回注册结果");
    const std::string json = server.list_tools_json();
    std::cout << json << '\n';
    Check(json.find("self.device.configure") != std::string::npos && json.find("inputSchema") != std::string::npos &&
              json.find("\"boolean\"") != std::string::npos && json.find("\"integer\"") != std::string::npos &&
              json.find("\"string\"") != std::string::npos && json.find("\"minimum\"") != std::string::npos &&
              json.find("\"maximum\"") != std::string::npos,
          "tools/list JSON 应包含完整的工具输入 Schema");
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
