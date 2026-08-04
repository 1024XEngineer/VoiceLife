#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/tool.h"

namespace voicelife::mcp {

/// 当前工具参数契约支持的数据类型，用于生成发送给模型的输入 Schema。
enum class ToolInputType {
    kString,
    kInteger,
    kBoolean,
};

/// 单个工具入参的声明信息。
struct ToolInputField {
    std::string name;
    ToolInputType type = ToolInputType::kString;
    bool required = false;
    std::optional<ToolValue> default_value;
    std::string description;
};

/// 可公开查询和导出的工具定义，不包含本地执行回调。
struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolInputField> input;
};

/// 模型发起工具调用时执行的本地回调。
using ToolHandler = std::function<ToolResult(const ToolCall&)>;

/// 按名称查询工具的结果；未找到时 tool 为空且 found 为 false。
struct GetToolResult {
    std::optional<ToolDefinition> tool;
    bool found = false;
};

/// 工具注册中心的完整列表及其数量。
struct ListToolsResult {
    std::vector<ToolDefinition> tools;
    std::size_t total = 0;
};

}  // namespace voicelife::mcp
