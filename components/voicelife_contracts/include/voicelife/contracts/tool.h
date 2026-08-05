#pragma once

#include <string>
#include <unordered_map>
#include <variant>

#include "voicelife/contracts/status.h"

namespace voicelife {

/// 工具调用参数当前支持的运行时值类型。
using ToolValue = std::variant<bool, int64_t, std::string>;
using ToolArguments = std::unordered_map<std::string, ToolValue>;

/// 描述一次进入设备侧的工具调用。
struct ToolCall {
    std::string request_id;
    std::string name;
    ToolArguments arguments;
};

/// 保存工具调用的状态和具名输出值。
struct ToolResult {
    Status status;
    std::unordered_map<std::string, std::string> output;
};

}  // namespace voicelife
