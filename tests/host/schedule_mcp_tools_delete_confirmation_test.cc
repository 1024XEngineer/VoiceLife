#include <string>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/mcp/schedule_mcp_tools.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ToolResult;
using voicelife::mcp::McpServer;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

std::string OutputString(const ToolResult& result, const std::string& key) {
    if (!result.output.IsObject()) return {};
    for (const auto& field : *result.output.object) {
        if (field.first == key && field.second->IsString()) return field.second->string;
    }
    return {};
}

ToolResult Call(McpServer& server, std::string request_id, voicelife::ToolArguments arguments) {
    return server.call(
        {.request_id = std::move(request_id), .name = "schedule.delete", .arguments = std::move(arguments)});
}

}  // namespace

int main() {
    InMemoryScheduleRepository schedules;
    ScheduleService service(schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service).ok(), "日程工具应注册成功");

    const ToolResult purple = server.call({
        .request_id = "create-purple",
        .name = "schedule.create",
        .arguments = {{"event", std::string("带紫色水彩笔")}, {"start_time", std::string("2026-08-27 14:13:00")}},
    });
    const ToolResult homework = server.call({
        .request_id = "create-homework",
        .name = "schedule.create",
        .arguments = {{"event", std::string("带科学作业去学校")}, {"start_time", std::string("2026-08-27 15:00:00")}},
    });
    Check(OutputString(purple, "status") == "success" && OutputString(homework, "status") == "success",
          "确认测试日程应创建成功");

    const ToolResult missing_confirmation = Call(server, "delete-missing-confirmation", {{"schedule_id", int64_t{1}}});
    Check(OutputString(missing_confirmation, "status") == "failure", "缺少目标确认的删除必须失败");
    Check(schedules.FindSchedule(1).value->status == ScheduleStatus::kActive, "缺少目标确认时不得取消目标日程");

    const ToolResult wrong_id = Call(server, "delete-wrong-id",
                                     {{"schedule_id", int64_t{2}},
                                      {"expected_event", std::string("带紫色水彩笔")},
                                      {"expected_start_time", std::string("2026-08-27 14:13:00")}});
    Check(OutputString(wrong_id, "status") == "failure", "错误 ID 与查询目标不一致时必须失败");
    Check(schedules.FindSchedule(1).value->status == ScheduleStatus::kActive &&
              schedules.FindSchedule(2).value->status == ScheduleStatus::kActive,
          "错误 ID 时不得取消任何日程");

    const ToolResult wrong_id_update = server.call({
        .request_id = "update-cancel-wrong-id",
        .name = "schedule.update",
        .arguments = {{"schedule_id", int64_t{2}},
                      {"expected_event", std::string("带紫色水彩笔")},
                      {"expected_start_time", std::string("2026-08-27 14:13:00")},
                      {"status", std::string("cancelled")}},
    });
    Check(OutputString(wrong_id_update, "status") == "failure", "update 取消遇到错误 ID 也必须失败");
    Check(schedules.FindSchedule(1).value->status == ScheduleStatus::kActive &&
              schedules.FindSchedule(2).value->status == ScheduleStatus::kActive,
          "update 取消遇到错误 ID 时不得取消任何日程");

    const ToolResult correct_target = Call(server, "delete-correct-target",
                                           {{"schedule_id", int64_t{1}},
                                            {"expected_event", std::string("带紫色水彩笔")},
                                            {"expected_start_time", std::string("2026-08-27 14:13:00")}});
    Check(OutputString(correct_target, "status") == "success", "正确 ID 和确认内容应取消目标日程");
    Check(schedules.FindSchedule(1).value->status == ScheduleStatus::kCancelled &&
              schedules.FindSchedule(2).value->status == ScheduleStatus::kActive,
          "正确删除只能取消已确认目标");
    return 0;
}
