#include "voicelife/mcp/mcp_tool_gateway.h"

#include <charconv>
#include <string_view>

namespace voicelife::mcp {
namespace {

constexpr std::string_view kCreateSchedule = "voicelife.schedule.create";

Result<int64_t> ParseTimestamp(const ToolArguments& arguments, const char* key, bool required) {
    const auto value = arguments.find(key);
    if (value == arguments.end()) {
        return required ? Result<int64_t>::Failure(ErrorCode::kInvalidArgument, std::string("缺少参数：") + key)
                        : Result<int64_t>::Success(0);
    }
    int64_t timestamp = 0;
    const char* begin = value->second.data();
    const char* end = begin + value->second.size();
    const auto parsed = std::from_chars(begin, end, timestamp);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return Result<int64_t>::Failure(ErrorCode::kInvalidArgument, std::string("时间参数格式错误：") + key);
    }
    return Result<int64_t>::Success(timestamp);
}

ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

}  // namespace

std::vector<ToolDefinition> McpToolGateway::ListTools() const {
    return {{
        .name = std::string(kCreateSchedule),
        .description = "创建一条本地日程，并原子注册对应定时任务",
        .required_arguments = {"title", "starts_at"},
    }};
}

ToolResult McpToolGateway::Call(const ToolCall& call) {
    if (call.request_id.empty()) {
        return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具调用缺少 request_id"));
    }
    if (call.name != kCreateSchedule) {
        return Failure(Status::Error(ErrorCode::kNotFound, "工具不存在：" + call.name));
    }
    const auto title = call.arguments.find("title");
    if (title == call.arguments.end()) {
        return Failure(Status::Error(ErrorCode::kInvalidArgument, "缺少参数：title"));
    }
    const auto starts_at = ParseTimestamp(call.arguments, "starts_at", true);
    if (!starts_at.ok()) {
        return Failure(starts_at.status);
    }
    const auto ends_at = ParseTimestamp(call.arguments, "ends_at", false);
    if (!ends_at.ok()) {
        return Failure(ends_at.status);
    }
    const auto zone = call.arguments.find("time_zone");
    schedule::CreateScheduleCommand command{
        .request_id = call.request_id,
        .title = title->second,
        .starts_at = *starts_at.value,
        .ends_at = *ends_at.value,
        .time_zone = zone == call.arguments.end() ? "Asia/Shanghai" : zone->second,
    };
    const auto result = calendar_.CreateSchedule(command);
    if (!result.ok()) {
        return Failure(result.status);
    }
    return {
        .status = Status::Ok(),
        .output =
            {
                {"schedule_id", result.value->schedule_id},
                {"timing_task_id", result.value->timing_task_id},
                {"duplicate", result.value->duplicate ? "true" : "false"},
                {"notification_accepted", result.value->notification_accepted ? "true" : "false"},
            },
    };
}

}  // namespace voicelife::mcp
