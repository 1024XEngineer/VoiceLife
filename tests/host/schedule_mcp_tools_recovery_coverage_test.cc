#include <optional>
#include <string>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/mcp/schedule_mcp_tools.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::mcp::McpServer;
using voicelife::schedule::DateTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleService;
using voicelife::schedule::ScheduleService;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;

namespace {

class FailingExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    voicelife::Result<ScheduleException> Upsert(const ScheduleException& exception) override {
        (void)exception;
        return voicelife::Result<ScheduleException>::Failure(ErrorCode::kUnavailable, "例外仓储暂不可用");
    }

    [[nodiscard]] voicelife::Result<std::vector<ScheduleException>> FindByRule(
        voicelife::schedule::ScheduleRuleId rule_id) const override {
        (void)rule_id;
        return voicelife::Result<std::vector<ScheduleException>>::Failure(ErrorCode::kUnavailable, "例外仓储暂不可用");
    }

    [[nodiscard]] voicelife::Result<std::optional<ScheduleException>> FindByRuleAndTime(
        voicelife::schedule::ScheduleRuleId rule_id, DateTime original_start_time) const override {
        (void)rule_id;
        (void)original_start_time;
        return voicelife::Result<std::optional<ScheduleException>>::Failure(ErrorCode::kUnavailable,
                                                                            "例外仓储暂不可用");
    }

    Status DeleteFuture(voicelife::schedule::ScheduleRuleId rule_id, DateTime after) override {
        (void)rule_id;
        (void)after;
        return Status::Error(ErrorCode::kUnavailable, "例外仓储暂不可用");
    }
};

class FailingRuleRepository final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    voicelife::Result<ScheduleRule> Insert(const ScheduleRule& rule) override {
        (void)rule;
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    Status Update(const ScheduleRule& rule) override {
        (void)rule;
        return Status::Error(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    [[nodiscard]] voicelife::Result<std::vector<ScheduleRule>> FindAll() const override {
        return voicelife::Result<std::vector<ScheduleRule>>::Failure(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    [[nodiscard]] voicelife::Result<ScheduleRule> FindById(voicelife::schedule::ScheduleRuleId id) const override {
        (void)id;
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    voicelife::Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule& rule,
                                                            const std::optional<Schedule>& first_instance) override {
        (void)rule;
        (void)first_instance;
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    voicelife::Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule& rule,
                                                     const std::optional<Schedule>& first_instance) override {
        (void)rule;
        (void)first_instance;
        return voicelife::Result<ScheduleRule>::Failure(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    Status CancelRuleAndInstances(voicelife::schedule::ScheduleRuleId id,
                                  int64_t& cancelled_instance_count) override {
        (void)id;
        (void)cancelled_instance_count;
        return Status::Error(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }

    voicelife::Result<Schedule> CreateNextInstance(const Schedule& schedule,
                                                   const std::optional<ScheduleException>& linked_exception) override {
        (void)schedule;
        (void)linked_exception;
        return voicelife::Result<Schedule>::Failure(ErrorCode::kUnavailable, "规则仓储暂不可用");
    }
};

void CheckCreateFailurePropagates() {
    InMemoryScheduleRepository schedules;
    FailingExceptionRepository exceptions;
    FailingRuleRepository rules;
    ScheduleService service(schedules);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service).ok(), "日程工具应注册成功");

    const auto response = server.call({.request_id = "create-failure",
                                       .name = "schedule.create",
                                       .arguments = {{"event", std::string("创建会议")},
                                                     {"start_time", std::string("2030-03-18 09:00:00")}}});
    Check(response.status.ok(), "工具应返回结构化结果");
    Check(response.output.IsObject(), "失败结果应带结构化对象输出");
    Check(response.output.kind == voicelife::ToolOutputValue::Kind::kObject,
          "创建失败应返回对象输出");
}

void CheckQueryFailurePropagates() {
    InMemoryScheduleRepository schedules;
    FailingExceptionRepository exceptions;
    FailingRuleRepository rules;
    ScheduleService service(schedules);
    ScheduleRuleService rule_service(rules, exceptions, schedules);
    McpServer server;
    Check(voicelife::mcp::RegisterScheduleMcpTools(server, service, rule_service).ok(), "日程工具应注册成功");

    const auto response = server.call({.request_id = "query-failure",
                                       .name = "schedule.query",
                                       .arguments = {{"keyword", std::string("会议")}}});
    Check(response.status.ok(), "查询失败应返回结构化结果");
    Check(response.output.IsObject(), "查询失败应带结构化对象输出");
    Check(response.output.kind == voicelife::ToolOutputValue::Kind::kObject,
          "查询失败应返回对象输出");
}

}  // namespace

int main() {
    CheckCreateFailurePropagates();
    CheckQueryFailurePropagates();
    return 0;
}
