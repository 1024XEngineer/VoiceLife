#include "voicelife/mcp/mcp_tool_gateway.h"

#include <optional>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::ToolCall;
using voicelife::application::CreateScheduleOutcome;
using voicelife::test::Check;

namespace {

class RecordingCreateSchedule final : public voicelife::application::CreateScheduleUseCase {
   public:
    Result<CreateScheduleOutcome> CreateSchedule(const voicelife::schedule::CreateScheduleCommand& command) override {
        last_command = command;
        ++calls;
        return result;
    }

    Result<CreateScheduleOutcome> result = Result<CreateScheduleOutcome>::Success({
        .schedule_id = "schedule-1",
        .timing_task_id = "task-1",
        .duplicate = false,
        .notification_accepted = true,
    });
    std::optional<voicelife::schedule::CreateScheduleCommand> last_command;
    int calls = 0;
};

}  // namespace

int main() {
    RecordingCreateSchedule use_case;
    voicelife::mcp::McpToolGateway gateway(use_case);

    Check(gateway.ListTools().size() == 1, "MCP 只暴露当前已实现的工具");
    ToolCall call{
        .request_id = "request-1",
        .name = "voicelife.schedule.create",
        .arguments =
            {
                {"title", "架构评审"},
                {"starts_at", "1785747600"},
                {"ends_at", "1785751200"},
                {"time_zone", "Asia/Shanghai"},
            },
    };
    const auto result = gateway.Call(call);
    Check(result.status.ok(), "合法 MCP 工具调用应成功");
    Check(use_case.calls == 1 && use_case.last_command.has_value(), "MCP 应调用抽象 Use Case Port");
    Check(use_case.last_command->starts_at == 1785747600, "MCP 应解析时间参数");
    Check(result.output.at("notification_accepted") == "true", "MCP 应映射用例输出");

    auto invalid = call;
    invalid.request_id.clear();
    Check(gateway.Call(invalid).status.code == ErrorCode::kInvalidArgument, "工具调用必须携带 request_id");

    invalid = call;
    invalid.name = "voicelife.unknown";
    Check(gateway.Call(invalid).status.code == ErrorCode::kNotFound, "未知工具应返回 not_found");

    invalid = call;
    invalid.arguments.erase("title");
    Check(gateway.Call(invalid).status.code == ErrorCode::kInvalidArgument, "创建日程必须携带标题");

    invalid = call;
    invalid.arguments["starts_at"] = "not-a-number";
    Check(gateway.Call(invalid).status.code == ErrorCode::kInvalidArgument, "非法时间格式应在 Adapter 边界拒绝");
    Check(use_case.calls == 1, "边界校验失败时不能进入 Use Case");
    return 0;
}
