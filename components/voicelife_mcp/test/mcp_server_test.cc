#include "voicelife/mcp/mcp_server.h"

#include <iostream>
#include <string>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolResult;
using voicelife::mcp::McpServer;
using voicelife::mcp::Property;
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
                               captured_value = properties.value<int64_t>("level");
                               return ToolResult{.status = Status::Ok(), .output = {}};
                           });
}

}  // namespace

/**
 * @brief 验证 MCP 工具注册、参数校验、调用和列表序列化能力。
 * @return 全部断言通过时返回 0。
 */
int main() {
    McpServer server;
    Check(server.list_tools().total == 0, "MCP 服务初始不应包含工具");

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

    Check(server.call({
                          .request_id = "request-3",
                          .name = "self.device.configure",
                          .arguments = {{"level", std::string("42")}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "参数类型不匹配时应拒绝调用");
    Check(server.call({
                          .request_id = "request-4",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{101}}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "整数参数超出范围时应拒绝调用");
    Check(server.call({
                          .request_id = "request-5",
                          .name = "self.device.configure",
                          .arguments = {{"level", int64_t{10}}, {"unknown", true}},
                      })
                  .status.code == ErrorCode::kInvalidArgument,
          "未定义参数应被拒绝");

    const auto listed = server.list_tools();
    Check(listed.total == 1 && listed.tools.front().name == "self.device.configure", "工具列表应返回注册结果");
    const std::string json = server.list_tools_json();
    std::cout << json << '\n';
    Check(json.find("self.device.configure") != std::string::npos && json.find("inputSchema") != std::string::npos,
          "tools/list JSON 应包含工具名称和输入 Schema");
    return 0;
}
