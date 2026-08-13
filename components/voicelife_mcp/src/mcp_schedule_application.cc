#include "voicelife/mcp/mcp_schedule_application.h"

#include <utility>

#include "voicelife/contracts/json.h"
#include "voicelife/mcp/schedule_tools.h"
#include "voicelife/schedule/schedule_repository.h"

namespace voicelife::mcp {

McpScheduleApplication::McpScheduleApplication(schedule::ScheduleRepository& repository)
    : schedule_service_(repository) {}

McpScheduleApplication::~McpScheduleApplication() { executor_.Stop(); }

Status McpScheduleApplication::Initialize() {
    if (initialized_) return Status::Ok();
    const Status status = RegisterScheduleTools(server_, schedule_service_);
    if (!status.ok()) return status;
    const Status executor_status = executor_.Start();
    if (!executor_status.ok()) return executor_status;
    initialized_ = true;
    return status;
}

Status McpScheduleApplication::SubmitJsonRpc(std::string_view request, McpJsonRpcResponseSink response_sink) {
    if (!initialized_) {
        return Status::Error(ErrorCode::kUnavailable, "MCP 应用尚未初始化");
    }
    const std::string copied_request(request);
    const bool is_tool_call = IsToolCall(copied_request);
    if (is_tool_call && observer_) observer_(true, false, {});
    (void)executor_.Submit(copied_request, [this, copied_request, is_tool_call,
                                            response_sink = std::move(response_sink)](auto response) mutable {
        if (!response.ok()) response = JsonRpcEndpoint::UnavailableResponse(copied_request, "设备 MCP 执行超时");
        if (is_tool_call && observer_) {
            const bool success = ResponseSucceeded(response);
            observer_(false, success, UserSummary(copied_request, response));
        }
        response_sink(std::move(response));
    });
    return Status::Ok();
}

Result<std::string> McpScheduleApplication::ExecuteJsonRpc(std::string_view request) const {
    if (!initialized_) return Result<std::string>::Failure(ErrorCode::kUnavailable, "MCP 应用尚未初始化");
    return endpoint_.Handle(request);
}

bool McpScheduleApplication::IsToolCall(std::string_view request) const {
    JsonValue parsed;
    const Status status = ParseJson(request, parsed);
    const JsonValue* method = status.ok() && parsed.IsObject() ? parsed.Get("method") : nullptr;
    return method != nullptr && method->IsString() && method->string == "tools/call";
}

std::string McpScheduleApplication::UserSummary(std::string_view request, const Result<std::string>& response) const {
    std::string tool_name;
    JsonValue request_json;
    if (ParseJson(request, request_json).ok() && request_json.IsObject()) {
        const JsonValue* params = request_json.Get("params");
        const JsonValue* name = params == nullptr ? nullptr : params->Get("name");
        if (name != nullptr && name->IsString()) tool_name = name->string;
    }
    const bool success = ResponseSucceeded(response);
    if (tool_name == "schedule.create") return success ? "日程已创建" : "日程创建失败";
    if (tool_name == "schedule.query") return success ? "日程查询完成" : "日程查询失败";
    return "日程操作失败";
}

bool McpScheduleApplication::ResponseSucceeded(const Result<std::string>& response) const {
    if (!response.ok() || !response.value.has_value()) return false;
    JsonValue response_json;
    return ParseJson(*response.value, response_json).ok() && response_json.IsObject() &&
           response_json.Get("error") == nullptr;
}

void McpScheduleApplication::SetExecutionObserver(ExecutionObserver observer) { observer_ = std::move(observer); }

}  // namespace voicelife::mcp
