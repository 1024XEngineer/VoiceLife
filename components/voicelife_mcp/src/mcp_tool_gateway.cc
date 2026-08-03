#include "voicelife/mcp/mcp_tool_gateway.h"

#include <unordered_set>
#include <utility>

namespace voicelife::mcp {
namespace {

// 将状态统一转换为无输出内容的工具调用失败结果。
ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

// 在写入注册中心前校验工具定义，防止产生不可调用或 Schema 冲突的注册项。
Status ValidateDefinition(const ToolDefinition& definition, const ToolHandler& handler) {
    if (definition.name.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具名称不能为空");
    }
    if (definition.description.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具描述不能为空");
    }
    if (!handler) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具 handler 不能为空");
    }

    std::unordered_set<std::string> input_names;
    for (const auto& field : definition.input) {
        if (field.name.empty()) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具入参名称不能为空");
        }
        if (!input_names.insert(field.name).second) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具入参名称重复：" + field.name);
        }
    }
    return Status::Ok();
}

}  // namespace

Status McpToolGateway::register_tool(ToolDefinition definition, ToolHandler handler) {
    const Status validation = ValidateDefinition(definition, handler);
    if (!validation.ok()) {
        return validation;
    }
    if (tools_.contains(definition.name)) {
        return Status::Error(ErrorCode::kAlreadyExists, "工具已注册：" + definition.name);
    }

    std::string name = definition.name;
    tools_.emplace(name, RegisteredTool{.definition = std::move(definition), .handler = std::move(handler)});
    registration_order_.push_back(std::move(name));
    return Status::Ok();
}

GetToolResult McpToolGateway::get_tool(std::string_view name) const {
    const auto registered = tools_.find(std::string(name));
    if (registered == tools_.end()) {
        return {};
    }
    return {.tool = registered->second.definition, .found = true};
}

ListToolsResult McpToolGateway::list_tools() const {
    ListToolsResult result;
    result.tools.reserve(registration_order_.size());
    for (const auto& name : registration_order_) {
        result.tools.push_back(tools_.at(name).definition);
    }
    result.total = result.tools.size();
    return result;
}

ToolResult McpToolGateway::call(const ToolCall& call) const {
    if (call.request_id.empty()) {
        return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具调用缺少 request_id"));
    }
    const auto registered = tools_.find(call.name);
    if (registered == tools_.end()) {
        return Failure(Status::Error(ErrorCode::kNotFound, "工具不存在：" + call.name));
    }
    return registered->second.handler(call);
}

}  // namespace voicelife::mcp
