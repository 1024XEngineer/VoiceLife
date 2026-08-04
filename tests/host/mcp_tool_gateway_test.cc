#include "voicelife/mcp/mcp_tool_gateway.h"

#include <string>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolCall;
using voicelife::ToolResult;
using voicelife::mcp::McpToolGateway;
using voicelife::mcp::ToolDefinition;
using voicelife::mcp::ToolInputField;
using voicelife::mcp::ToolInputType;
using voicelife::test::Check;

namespace {

ToolDefinition EchoDefinition(std::string name = "voicelife.test.echo") {
    return {
        .name = std::move(name),
        .description = "回显指定文本",
        .input =
            {
                ToolInputField{
                    .name = "text",
                    .type = ToolInputType::kString,
                    .required = true,
                    .default_value = std::nullopt,
                    .description = "需要回显的文本",
                },
            },
    };
}

ToolDefinition DefaultEchoDefinition() {
    auto definition = EchoDefinition("voicelife.test.default-echo");
    definition.input.front().required = false;
    definition.input.front().default_value = std::string("default");
    return definition;
}

ToolDefinition IntegerDefinition() {
    return {
        .name = "voicelife.test.integer",
        .description = "读取整数",
        .input = {ToolInputField{
            .name = "value",
            .type = ToolInputType::kInteger,
            .required = true,
            .default_value = std::nullopt,
            .description = "整数值",
        }},
    };
}

ToolResult OkResult(const ToolCall& call) {
    return {.status = Status::Ok(), .output = {{"echo", std::get<std::string>(call.arguments.at("text"))}}};
}

}  // namespace

int main() {
    McpToolGateway gateway;
    Check(gateway.list_tools().total == 0, "工具注册中心初始应为空");
    Check(!gateway.get_tool("voicelife.test.echo").found, "未注册工具不应被查询到");

    int handler_calls = 0;
    const auto registered = gateway.register_tool(EchoDefinition(), [&handler_calls](const ToolCall& call) {
        ++handler_calls;
        return OkResult(call);
    });
    Check(registered.ok(), "合法工具应注册成功");

    const auto found = gateway.get_tool("voicelife.test.echo");
    Check(found.found && found.tool.has_value(), "应能按名称查询已注册工具");
    Check(found.tool->input.size() == 1 && found.tool->input.front().required, "查询结果应包含完整入参定义");

    Check(gateway.register_tool(EchoDefinition(), OkResult).code == ErrorCode::kAlreadyExists, "同名工具不能重复注册");
    Check(gateway.register_tool(EchoDefinition("voicelife.test.second"), OkResult).ok(), "应能注册不同名称的工具");

    const auto listed = gateway.list_tools();
    Check(listed.total == 2 && listed.tools.size() == 2, "工具列表数量应准确");
    Check(listed.tools[0].name == "voicelife.test.echo" && listed.tools[1].name == "voicelife.test.second",
          "工具列表应保持注册顺序");

    const auto called = gateway.call({
        .request_id = "request-1",
        .name = "voicelife.test.echo",
        .arguments = {{"text", "hello"}},
    });
    Check(called.status.ok() && called.output.at("echo") == "hello", "工具调用应分发给已注册 handler");
    Check(handler_calls == 1, "每次工具调用只应执行一次 handler");

    Check(gateway.call({
                           .request_id = "request-2",
                           .name = "voicelife.test.echo",
                           .arguments = {},
                       })
                  .status.code == ErrorCode::kInvalidArgument,
          "缺少必填参数时 Gateway 应拒绝调用");
    Check(gateway.call({
                           .request_id = "request-3",
                           .name = "voicelife.test.echo",
                           .arguments = {{"text", "hello"}, {"extra", "value"}},
                       })
                  .status.code == ErrorCode::kInvalidArgument,
          "携带未定义参数时 Gateway 应拒绝调用");

    Check(gateway.register_tool(DefaultEchoDefinition(), OkResult).ok(), "应能注册带默认值的工具");
    const auto default_result = gateway.call({
        .request_id = "request-4",
        .name = "voicelife.test.default-echo",
        .arguments = {},
    });
    Check(default_result.status.ok() && default_result.output.at("echo") == "default", "Gateway 应补齐默认参数");

    Check(gateway
              .register_tool(
                  IntegerDefinition(),
                  [](const ToolCall& call) {
                      return ToolResult{
                          .status = Status::Ok(),
                          .output = {{"value", std::to_string(std::get<int64_t>(call.arguments.at("value")))}}};
                  })
              .ok(),
          "应能注册整数参数工具");
    Check(gateway
              .call({.request_id = "request-5", .name = "voicelife.test.integer", .arguments = {{"value", int64_t{7}}}})
              .status.ok(),
          "整数参数应通过类型校验");
    Check(gateway.call({.request_id = "request-6",
                        .name = "voicelife.test.integer",
                        .arguments = {{"value", std::string("7")}}})
                  .status.code == ErrorCode::kInvalidArgument,
          "字符串不能伪装成整数参数");

    Check(gateway.call({.request_id = "", .name = "voicelife.test.echo", .arguments = {}}).status.code ==
              ErrorCode::kInvalidArgument,
          "工具调用必须携带 request_id");
    Check(gateway.call({.request_id = "request-2", .name = "voicelife.unknown", .arguments = {}}).status.code ==
              ErrorCode::kNotFound,
          "调用未注册工具应返回 not_found");

    Check(gateway.register_tool({}, OkResult).code == ErrorCode::kInvalidArgument, "空工具定义应被拒绝");
    Check(gateway.register_tool(EchoDefinition("voicelife.test.no-handler"), {}).code == ErrorCode::kInvalidArgument,
          "空 handler 应被拒绝");

    auto duplicate_input = EchoDefinition("voicelife.test.duplicate-input");
    duplicate_input.input.push_back(duplicate_input.input.front());
    Check(gateway.register_tool(std::move(duplicate_input), OkResult).code == ErrorCode::kInvalidArgument,
          "重复入参名称应被拒绝");
    return 0;
}
