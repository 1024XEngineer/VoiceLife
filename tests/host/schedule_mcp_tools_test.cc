#include "schedule_mcp_tools.h"

#include "support/test_support.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolCall;
using voicelife::mcp::McpServer;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;

int main() {
    McpServer server;
    ScheduleService service;
    Check(voicelife::runtime::RegisterScheduleMcpTools(server, service).ok(), "日程工具应注册成功");

    const auto listed = server.list_tools();
    Check(listed.total == 2, "MVP 只应注册两个日程工具");
    Check(listed.tools[0].name == "schedule.create" && listed.tools[1].name == "schedule.query",
          "日程工具应保持稳定注册顺序");

    const auto created = server.call({
        .request_id = "create-1",
        .name = "schedule.create",
        .arguments = {{"event", std::string("评审 Linx")}, {"start_time", int64_t{1'900'000'000}}},
    });
    Check(created.status.ok() && created.output.at("event") == "评审 Linx", "创建工具应调用 ScheduleService");

    const auto queried = server.call({
        .request_id = "query-1",
        .name = "schedule.query",
        .arguments = {{"status", std::string("active")}, {"limit", int64_t{5}}},
    });
    Check(queried.status.ok() && queried.output.contains("total"), "查询工具应返回总数");

    const auto invalid = server.call({
        .request_id = "create-2",
        .name = "schedule.create",
        .arguments = {{"event", std::string("错误")}, {"start_time", std::string("not-unix")}},
    });
    Check(invalid.status.code == ErrorCode::kInvalidArgument, "错误时间类型应在 Gateway 边界被拒绝");
    return 0;
}
