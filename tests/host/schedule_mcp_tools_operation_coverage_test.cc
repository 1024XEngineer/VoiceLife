#define main ExistingScheduleMcpToolsReminderTestMain
#include "schedule_mcp_tools_test.cc"
#undef main

#include "im_runtime_test_support.h"
#include "voicelife/schedule/schedule_operation_service.h"

using voicelife::ErrorCode;
using voicelife::im::ImTransportStatus;
using voicelife::schedule::ScheduleOperationService;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;
using voicelife::test::im_runtime_support::RuntimeFixture;

namespace {

void CheckOperationQueryPaths() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleOperationService operation_service(schedules);
    ScheduleService service(schedules, &operation_service);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service, operation_service).ok(),
          "操作记录工具应注册成功");

    const auto created = server.call({
        .request_id = "operation-create",
        .name = "schedule.create",
        .arguments = {{"event", std::string("操作记录日程")}},
    });
    Check(created.status.ok() && OutputString(created, "status") == "success", "创建操作记录样本应成功");

    const auto query = server.call({
        .request_id = "operation-query",
        .name = "schedule.operation_query",
        .arguments = {{"entity_type", std::string("schedule")},
                      {"type", std::string("create")},
                      {"keyword", std::string("操作记录")}},
    });
    Check(query.status.ok() && OutputString(query, "status") == "success", "操作记录查询应成功");

    const auto invalid_entity = server.call({
        .request_id = "operation-invalid-entity",
        .name = "schedule.operation_query",
        .arguments = {{"entity_type", std::string("invalid")}},
    });
    Check(OutputString(invalid_entity, "status") == "failure", "非法 entity_type 应返回失败");

    const auto invalid_type = server.call({
        .request_id = "operation-invalid-type",
        .name = "schedule.operation_query",
        .arguments = {{"type", std::string("invalid")}},
    });
    Check(OutputString(invalid_type, "status") == "failure", "非法 type 应返回失败");

    schedules.FailNextFindOperations(voicelife::Status::Error(ErrorCode::kUnavailable, "操作查询失败"));
    const auto failed = server.call({
        .request_id = "operation-query-failed",
        .name = "schedule.operation_query",
        .arguments = {},
    });
    Check(OutputString(failed, "status") == "failure", "操作记录仓储失败应返回失败");
}

void CheckScheduleQueryReportingPaths() {
    InMemoryScheduleRepository schedules;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules(schedules, exceptions);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    ScheduleOperationService operation_service(schedules);
    ScheduleService service(schedules, &operation_service);
    RuntimeFixture runtime_fixture;
    Check(runtime_fixture.runtime.Start().ok(), "IM runtime 应进入探测状态");
    Check(runtime_fixture.runtime.ProbeGateway().status == ImTransportStatus::kHttpError,
          "测试 Gateway 探针应返回受控 404");
    Check(runtime_fixture.runtime.reporting_channel() != nullptr, "Gateway 探针成功后应创建上报通道");

    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service, operation_service, nullptr,
                                                   {.runtime = &runtime_fixture.runtime})
              .ok(),
          "查询上报上下文应注册成功");
    const auto created = server.call({
        .request_id = "reporting-sample",
        .name = "schedule.create",
        .arguments = {{"event", std::string("上报日程")}, {"start_time", std::string("2030-01-01 09:00:00")}},
    });
    Check(created.status.ok(), "上报测试样本创建应成功");

    runtime_fixture.transport->next_post_response = {
        .status = ImTransportStatus::kSuccess, .status_code = 200, .body = "{}", .message = {}};
    const auto submitted = server.call({
        .request_id = "reporting-submitted",
        .name = "schedule.query",
        .arguments = {{"keyword", std::string("上报")}},
    });
    Check(submitted.status.ok() && OutputString(submitted, "im_delivery") == "submitted" &&
              submitted.text_output.has_value() && submitted.text_output->find("已通过 IM 提交") != std::string::npos,
          "IM 上报成功应返回 submitted 状态和用户摘要");

    runtime_fixture.transport->next_post_response = {
        .status = ImTransportStatus::kNetworkFailure, .status_code = 0, .body = {}, .message = "network down"};
    const auto retryable = server.call({
        .request_id = "reporting-retryable",
        .name = "schedule.query",
        .arguments = {},
    });
    Check(retryable.status.ok() && OutputString(retryable, "im_delivery") == "retryable_failed" &&
              retryable.text_output.has_value() && retryable.text_output->find("可重试") != std::string::npos,
          "IM 上报失败应返回 retryable_failed 和可重试摘要");
}

}  // namespace

int main() {
    Check(ExistingScheduleMcpToolsReminderTestMain() == 0, "完整日程 MCP 覆盖测试应通过");
    CheckOperationQueryPaths();
    CheckScheduleQueryReportingPaths();
    return 0;
}
