#pragma once

#include <string>
#include <vector>

#include "voicelife/application/calendar_application.h"
#include "voicelife/contracts/tool.h"

namespace voicelife::mcp {

struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<std::string> required_arguments;
};

class McpToolGateway {
   public:
    explicit McpToolGateway(application::CalendarApplication& calendar) : calendar_(calendar) {}

    std::vector<ToolDefinition> ListTools() const;
    ToolResult Call(const ToolCall& call);

   private:
    application::CalendarApplication& calendar_;
};

}  // namespace voicelife::mcp
