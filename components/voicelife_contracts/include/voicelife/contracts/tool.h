#pragma once

#include <string>
#include <unordered_map>

#include "voicelife/contracts/status.h"

namespace voicelife {

using ToolArguments = std::unordered_map<std::string, std::string>;

struct ToolCall {
    std::string request_id;
    std::string name;
    ToolArguments arguments;
};

struct ToolResult {
    Status status;
    std::unordered_map<std::string, std::string> output;
};

}  // namespace voicelife
