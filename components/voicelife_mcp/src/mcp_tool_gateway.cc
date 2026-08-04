#include "voicelife/mcp/mcp_tool_gateway.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace voicelife::mcp {
namespace {

bool MatchesType(const ToolValue& value, ToolInputType type) {
    switch (type) {
        case ToolInputType::kString:
            return std::holds_alternative<std::string>(value);
        case ToolInputType::kInteger:
            return std::holds_alternative<int64_t>(value);
        case ToolInputType::kBoolean:
            return std::holds_alternative<bool>(value);
    }
    return false;
}

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

    if (definition.input_schema.type != "object") {
        return Status::Error(ErrorCode::kInvalidArgument, "工具 inputSchema.type 必须为 object");
    }
    std::unordered_set<std::string> required_names;
    for (const auto& name : definition.input_schema.required) {
        if (!required_names.insert(name).second) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具必填参数重复：" + name);
        }
        if (!definition.input_schema.properties.contains(name)) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具必填参数未定义：" + name);
        }
    }
    for (const auto& [name, field] : definition.input_schema.properties) {
        if (name.empty()) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具入参名称不能为空");
        }
        if (field.default_value.has_value() && !MatchesType(*field.default_value, field.type)) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具默认值类型错误：" + name);
        }
    }
    return Status::Ok();
}

// 校验调用参数，并补齐定义中声明的默认值后交给 handler。
Status NormalizeArguments(const ToolDefinition& definition, ToolCall& call) {
    for (const auto& [name, field] : definition.input_schema.properties) {
        const auto argument = call.arguments.find(name);
        if (argument == call.arguments.end()) {
            if (field.default_value.has_value()) {
                call.arguments.emplace(name, *field.default_value);
            } else if (std::find(definition.input_schema.required.begin(), definition.input_schema.required.end(), name) !=
                       definition.input_schema.required.end()) {
                return Status::Error(ErrorCode::kInvalidArgument, "缺少参数：" + name);
            }
        } else if (!MatchesType(argument->second, field.type)) {
            return Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + name);
        }
    }
    for (const auto& argument : call.arguments) {
        if (!definition.input_schema.properties.contains(argument.first)) {
            return Status::Error(ErrorCode::kInvalidArgument, "不支持的参数：" + argument.first);
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
    ToolCall normalized_call = call;
    const Status validation = NormalizeArguments(registered->second.definition, normalized_call);
    if (!validation.ok()) {
        return Failure(validation);
    }
    return registered->second.handler(normalized_call);
}

}  // namespace voicelife::mcp
