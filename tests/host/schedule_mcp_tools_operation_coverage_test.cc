#define main ExistingScheduleMcpToolsReminderTestMain
#include "schedule_mcp_tools_test.cc"
#undef main

#include "../../components/voicelife_mcp/src/tools/schedule_tool_output.h"
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

void CheckScheduleToolOutputBoundaryPaths() {
    using namespace voicelife::mcp::schedule_tool_output;
    using voicelife::schedule::DateTime;
    using voicelife::schedule::Frequency;
    using voicelife::schedule::MonthlyMode;
    using voicelife::schedule::OperationEntityType;
    using voicelife::schedule::ScheduleException;
    using voicelife::schedule::ScheduleOperationType;
    using voicelife::schedule::ScheduleRule;
    using voicelife::schedule::ScheduleStatus;

    Check(ParseDateTime("2024-02-29 23:59:59").has_value(), "合法闰日时间应解析成功");
    for (const char* invalid : {"", "2024-2-29 23:59:59", "2023-02-29 00:00:00", "2024-01-01 24:00:00",
                                "2024-01-01 00:00:60", "2024-01-01 00:00:00x"}) {
        Check(!ParseDateTime(invalid).has_value(), "非法日期时间应被拒绝");
    }
    Check(ParseLocalTime("09:08:07").has_value() && !ParseLocalTime("9:08:07").has_value(),
          "本地时间应严格使用两位字段");
    Check(ParseLocalDate("2024-02-29").has_value() && !ParseLocalDate("2024-02-30").has_value(),
          "本地日期应校验日历边界");

    for (const auto status : {ScheduleStatus::kActive, ScheduleStatus::kCancelled, ScheduleStatus::kCompleted})
        Check(StatusName(status) != nullptr, "全部日程状态均应有名称");
    Check(std::string(StatusName(static_cast<ScheduleStatus>(99))) == "active", "未知日程状态应安全回退");
    for (const auto frequency : {Frequency::kDaily, Frequency::kWeekly, Frequency::kMonthly, Frequency::kYearly})
        Check(FrequencyName(frequency) != nullptr, "全部周期频率均应有名称");
    Check(std::string(FrequencyName(static_cast<Frequency>(99))) == "daily", "未知周期频率应安全回退");
    Check(std::string(MonthlyModeName(MonthlyMode::kSpecificDay)) == "specific_day" &&
              std::string(MonthlyModeName(MonthlyMode::kLastDay)) == "last_day",
          "两种月模式均应有名称");
    for (const auto entity :
         {OperationEntityType::kSchedule, OperationEntityType::kRule, OperationEntityType::kException})
        Check(EntityTypeName(entity) != nullptr, "全部操作实体均应有名称");
    Check(std::string(EntityTypeName(static_cast<OperationEntityType>(99))) == "schedule", "未知操作实体应安全回退");
    for (const auto type :
         {ScheduleOperationType::kCreate, ScheduleOperationType::kUpdate, ScheduleOperationType::kDelete})
        Check(OperationTypeName(type) != nullptr, "全部操作类型均应有名称");
    Check(std::string(OperationTypeName(static_cast<ScheduleOperationType>(99))) == "create", "未知操作类型应安全回退");

    const DateTime instant{std::chrono::seconds{1'704'067'200}};
    ScheduleRule rule;
    rule.id = 9;
    rule.event = "周期输出";
    rule.freq_type = Frequency::kMonthly;
    rule.monthly_mode = MonthlyMode::kLastDay;
    rule.start_date = {2024, 1, 31};
    rule.start_time = {9, 0, 0};
    rule.end_time = voicelife::schedule::LocalTime{10, 0, 0};
    rule.end_date = voicelife::schedule::LocalDate{2024, 12, 31};
    rule.occurrence_count = 3;
    rule.weekdays_mask = 0x7f;
    rule.day_of_month = 31;
    rule.month_of_year = 1;
    rule.location = "会议室";
    rule.notes = "备注";
    Schedule schedule;
    schedule.id = 10;
    schedule.event = "单次输出";
    schedule.status = ScheduleStatus::kCompleted;
    schedule.start_time = instant;
    schedule.end_time = instant + std::chrono::hours{1};
    schedule.location = "地点";
    schedule.notes = "说明";
    schedule.rule_id = rule.id;
    Check(ScheduleOutput(schedule).IsObject() && ScheduleOutput(schedule, &rule).IsObject(),
          "日程输出应覆盖有无周期规则两条路径");
    Check(RuleOutput(rule).IsObject(), "规则输出应覆盖全部可选字段");
    Check(FutureOccurrenceOutput(rule, instant).IsObject(), "带结束时间的未来实例应生成对象");
    rule.end_time.reset();
    Check(FutureOccurrenceOutput(rule, instant).IsObject(), "无结束时间的未来实例应生成对象");
    Check(FutureOccurrencesOutput(rule, {instant, instant + std::chrono::hours{24}}).size() == 2,
          "未来实例数组应保留全部元素");
    Check(ScheduleArrayOutput({schedule}).size() == 1, "日程数组输出应保留元素");

    ScheduleException exception;
    exception.id = 11;
    exception.rule_id = rule.id;
    exception.original_start_time = instant;
    exception.type = voicelife::schedule::ExceptionType::kSkip;
    Check(ExceptionOutput(exception).IsObject(), "跳过例外应生成对象");
    exception.type = voicelife::schedule::ExceptionType::kModify;
    exception.schedule_id = 12;
    exception.override_start_time = instant;
    exception.override_end_time = instant + std::chrono::hours{1};
    exception.override_event = "覆盖事件";
    exception.override_location = "覆盖地点";
    exception.override_notes = "覆盖说明";
    Check(ExceptionsOutput({exception}).size() == 1, "修改例外应覆盖全部覆盖字段");

    for (const JsonValue& value :
         {JsonValue{}, JsonValue::Bool(true), JsonValue::Number(4), JsonValue::String("text"),
          JsonValue::Array({JsonValue::Number(1)}), JsonValue::Object({{"key", JsonValue::String("value")}})})
        Check(JsonToToolOutputValue(value).kind != voicelife::ToolOutputValue::Kind::kNull ||
                  value.kind == JsonValue::Kind::kNull,
              "JsonValue 各类型均应可转换");
    Check(BeforeOutput(std::nullopt).kind == voicelife::ToolOutputValue::Kind::kNull, "缺失 before 快照应为空值");
    Check(BeforeOutput(std::string("bad-json")).kind == voicelife::ToolOutputValue::Kind::kNull,
          "非法 before 快照应为空值");
    Check(BeforeOutput(std::string("[]")).kind == voicelife::ToolOutputValue::Kind::kNull,
          "非对象 before 快照应为空值");
    Check(BeforeOutput(std::string(R"({"count":2,"nested":[true]})")).IsObject(), "合法 before 快照应递归转换为对象");

    voicelife::schedule::OperationRecord operation;
    operation.id = 13;
    operation.entity_type = OperationEntityType::kException;
    operation.type = ScheduleOperationType::kDelete;
    operation.entity_id = exception.id;
    operation.label = "例外操作";
    operation.operated_at = instant;
    operation.before = R"({"event":"before"})";
    Check(OperationOutput(operation).IsObject() && OperationArrayOutput({operation}).size() == 1,
          "操作记录输出应覆盖 before 快照和数组路径");
}

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
    CheckScheduleToolOutputBoundaryPaths();
    CheckOperationQueryPaths();
    CheckScheduleQueryReportingPaths();
    return 0;
}
