#pragma once

#include <string>
#include <unordered_map>
#include <variant>

#include "voicelife/contracts/status.h"

namespace voicelife {

/// 工具调用参数当前支持的运行时值类型。
using ToolValue = std::variant<bool, int64_t, std::string>;
using ToolArguments = std::unordered_map<std::string, ToolValue>;

/// Describes one incoming tool invocation.
struct ToolCall {
    std::string request_id;
    std::string name;
    ToolArguments arguments;
};

/// Contains the status and named output values of a tool invocation.
struct ToolResult {
    Status status;
    std::unordered_map<std::string, std::string> output;
};

}  // namespace voicelife
