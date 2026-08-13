#include "voicelife/mcp/schedule_mcp_tools.h"

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolCall;
using voicelife::mcp::McpServer;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

int main() {
    McpServer server;
    InMemoryScheduleRepository repository;
    ScheduleService service(repository);
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service).ok(), "日程工具应注册成功");

    const auto listed = server.list_tools();
    Check(listed.total == 4, "一次性日程应注册四个工具");
    Check(listed.tools[0].name == "schedule.create" && listed.tools[1].name == "schedule.query" &&
              listed.tools[2].name == "schedule.update" && listed.tools[3].name == "schedule.delete",
          "日程工具应保持稳定注册顺序");

    const auto created = server.call({
        .request_id = "create-1",
        .name = "schedule.create",
        .arguments = {{"event", std::string("评审 Linx")}, {"start_time", std::string("2030-03-18 00:00:00")}},
    });
    Check(created.status.ok() && created.output.IsObject(), "创建工具应返回结构化结果");

    const auto queried = server.call({
        .request_id = "query-1",
        .name = "schedule.query",
        .arguments = {{"status", std::string("active")}},
    });
    Check(queried.status.ok() && queried.output.IsObject(), "查询工具应返回结构化结果");

    const auto invalid = server.call({
        .request_id = "create-2",
        .name = "schedule.create",
        .arguments = {{"event", std::string("错误")}, {"start_time", std::string("not-unix")}},
    });
    Check(invalid.status.ok() && invalid.output.IsObject(), "错误时间格式应作为业务失败返回");
    return 0;
}
