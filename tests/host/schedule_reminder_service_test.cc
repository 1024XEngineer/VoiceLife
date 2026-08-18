#include "voicelife/schedule/schedule_reminder_service.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/in_memory_schedule_repository.h"
#include "support/test_support.h"
#include "voicelife/schedule/schedule_exception_repository.h"
#include "voicelife/schedule/schedule_rule_repository.h"
#include "voicelife/timing/timing_task.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::schedule::DateTime;
using voicelife::schedule::Frequency;
using voicelife::schedule::LocalDate;
using voicelife::schedule::LocalTime;
using voicelife::schedule::Schedule;
using voicelife::schedule::ScheduleException;
using voicelife::schedule::ScheduleReminderService;
using voicelife::schedule::ScheduleReminderSpeechPort;
using voicelife::schedule::ScheduleRule;
using voicelife::schedule::ScheduleRuleId;
using voicelife::schedule::ScheduleRuleService;
using voicelife::schedule::ScheduleService;
using voicelife::schedule::ScheduleStatus;
using voicelife::test::Check;
using voicelife::test::InMemoryScheduleRepository;
using voicelife::timing::InMemoryTimingTaskRunner;
using voicelife::timing::TriggerAt;

namespace {

DateTime At(int64_t seconds) { return DateTime{std::chrono::seconds{seconds}}; }
TriggerAt Trigger(int64_t seconds) { return TriggerAt{std::chrono::seconds{seconds}}; }

class FakeSpeech final : public ScheduleReminderSpeechPort {
   public:
    Status SpeakScheduleReminder(std::string_view text) override {
        texts.emplace_back(text);
        return next_status;
    }

    Status next_status = Status::Ok();
    std::vector<std::string> texts;
};

class FakeExceptionRepository final : public voicelife::schedule::ScheduleExceptionRepository {
   public:
    Result<ScheduleException> Upsert(const ScheduleException& exception) override {
        exceptions.push_back(exception);
        return Result<ScheduleException>::Success(exception);
    }

    Result<std::vector<ScheduleException>> FindByRule(ScheduleRuleId rule_id) const override {
        std::vector<ScheduleException> matched;
        for (const auto& exception : exceptions) {
            if (exception.rule_id == rule_id) matched.push_back(exception);
        }
        return Result<std::vector<ScheduleException>>::Success(std::move(matched));
    }

    Result<std::optional<ScheduleException>> FindByRuleAndTime(ScheduleRuleId rule_id,
                                                               DateTime original_start_time) const override {
        for (const auto& exception : exceptions) {
            if (exception.rule_id == rule_id && exception.original_start_time == original_start_time) {
                return Result<std::optional<ScheduleException>>::Success(exception);
            }
        }
        return Result<std::optional<ScheduleException>>::Success(std::nullopt);
    }

    Status DeleteFuture(ScheduleRuleId rule_id, DateTime after) override {
        (void)rule_id;
        (void)after;
        return Status::Ok();
    }

    std::vector<ScheduleException> exceptions;
};

class FakeRuleRepository final : public voicelife::schedule::ScheduleRuleRepository {
   public:
    explicit FakeRuleRepository(InMemoryScheduleRepository& schedules) : schedules_(schedules) {}

    Result<ScheduleRule> Insert(const ScheduleRule& rule) override {
        rules.push_back(rule);
        return Result<ScheduleRule>::Success(rule);
    }

    Status Update(const ScheduleRule& rule) override {
        for (ScheduleRule& current : rules) {
            if (current.id == rule.id) {
                current = rule;
                return Status::Ok();
            }
        }
        return Status::Error(ErrorCode::kNotFound, "规则不存在");
    }

    Result<std::vector<ScheduleRule>> FindAll() const override {
        return Result<std::vector<ScheduleRule>>::Success(rules);
    }

    Result<ScheduleRule> FindById(ScheduleRuleId id) const override {
        for (const auto& rule : rules) {
            if (rule.id == id) return Result<ScheduleRule>::Success(rule);
        }
        return Result<ScheduleRule>::Failure(ErrorCode::kNotFound, "规则不存在");
    }

    Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule& rule,
                                                 const std::optional<Schedule>& first_instance) override {
        (void)first_instance;
        return Insert(rule);
    }

    Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule& rule,
                                          const std::optional<Schedule>& first_instance) override {
        (void)first_instance;
        const Status status = Update(rule);
        return status.ok() ? Result<ScheduleRule>::Success(rule)
                           : Result<ScheduleRule>::Failure(status.code, status.message);
    }

    Status CancelRuleAndInstances(ScheduleRuleId id, int64_t& cancelled_instance_count) override {
        cancelled_instance_count = 0;
        (void)id;
        return Status::Ok();
    }

    Result<Schedule> CreateNextInstance(const Schedule& schedule,
                                        const std::optional<ScheduleException>& linked_exception) override {
        (void)linked_exception;
        ++create_next_calls;
        if (fail_create_next_count > 0) {
            --fail_create_next_count;
            return Result<Schedule>::Failure(ErrorCode::kInternal, "生成失败");
        }
        return schedules_.Insert(schedule);
    }

    std::vector<ScheduleRule> rules;
    int create_next_calls = 0;
    int fail_create_next_count = 0;

   private:
    InMemoryScheduleRepository& schedules_;
};

Schedule MakeSchedule(int64_t id, std::string event, std::optional<DateTime> start,
                      std::optional<ScheduleRuleId> rule_id = std::nullopt) {
    return {
        .id = id,
        .event = std::move(event),
        .start_time = start,
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .rule_id = rule_id,
        .reminder_task_id = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = At(900),
        .updated_at = At(900),
    };
}

ScheduleRule DailyRule(ScheduleRuleId id) {
    return {
        .id = id,
        .event = "每日提醒",
        .location = std::nullopt,
        .notes = std::nullopt,
        .freq_type = Frequency::kDaily,
        .interval_val = 1,
        .weekdays_mask = std::nullopt,
        .day_of_month = std::nullopt,
        .month_of_year = std::nullopt,
        .monthly_mode = std::nullopt,
        .start_time = LocalTime{8, 0, 0},
        .end_time = std::nullopt,
        .start_date = LocalDate{2026, 1, 1},
        .end_date = std::nullopt,
        .occurrence_count = std::nullopt,
        .status = ScheduleStatus::kActive,
        .created_at = At(900),
        .updated_at = At(900),
    };
}

struct Fixture {
    explicit Fixture(std::vector<Schedule> schedules, DateTime current = At(1'000))
        : repository(std::move(schedules)),
          rules(repository),
          rule_service(rules, exceptions, repository),
          schedule_service(repository),
          now(current),
          reminder(repository, schedule_service, rule_service, timing, speech, [this]() { return now; }) {}

    InMemoryScheduleRepository repository;
    FakeExceptionRepository exceptions;
    FakeRuleRepository rules;
    ScheduleRuleService rule_service;
    ScheduleService schedule_service;
    InMemoryTimingTaskRunner timing;
    FakeSpeech speech;
    DateTime now;
    ScheduleReminderService reminder;
};

void CheckFutureMemoAndExpiredRestoration() {
    Fixture fixture({
        MakeSchedule(1, "未来会议", At(1'100)),
        MakeSchedule(2, "备忘录", std::nullopt),
        MakeSchedule(3, "已过期", At(999)),
    });
    Check(fixture.reminder.Start().ok(), "启动应恢复未来提醒");
    Check(fixture.timing.ProcessPendingCommands(Trigger(1'000)) == 1, "只有未来有开始时间的日程应注册提醒");

    const auto future = fixture.repository.FindById(1);
    const auto memo = fixture.repository.FindById(2);
    const auto expired = fixture.repository.FindById(3);
    Check(future.ok() && future.value->reminder_task_id.has_value(), "未来日程应持久化提醒任务标识");
    Check(memo.ok() && !memo.value->reminder_task_id.has_value(), "备忘录不应注册提醒");
    Check(expired.ok() && expired.value->status == ScheduleStatus::kActive &&
              !expired.value->reminder_task_id.has_value(),
          "过期日程应保持 Active 且无提醒");

    const auto ran = fixture.timing.RunDueTasks(Trigger(1'100));
    Check(ran.processed_count == 1 && fixture.speech.texts.size() == 1 &&
              fixture.speech.texts.front() == "提醒：现在是「未来会议」时间了",
          "到点应使用约定模板提交 TTS");
    const auto completed = fixture.repository.FindById(1);
    Check(completed.ok() && completed.value->status == ScheduleStatus::kCompleted &&
              !completed.value->reminder_task_id.has_value(),
          "TTS 成功后应标记完成并清除任务标识");
}

void CheckSpeechFailureLeavesActive() {
    Fixture fixture({MakeSchedule(1, "失败提醒", At(1'100))});
    fixture.speech.next_status = Status::Error(ErrorCode::kUnavailable, "TTS 不可用");
    Check(fixture.reminder.Start().ok(), "失败测试应启动提醒服务");
    fixture.timing.RunDueTasks(Trigger(1'100));
    const auto stored = fixture.repository.FindById(1);
    Check(stored.ok() && stored.value->status == ScheduleStatus::kActive && !stored.value->reminder_task_id.has_value(),
          "TTS 失败后应保持 Active 并清除已终止任务标识");
}

void CheckCancellationAndRescheduleUseFreshIds() {
    Fixture fixture({MakeSchedule(1, "原提醒", At(1'100))});
    Check(fixture.reminder.Start().ok(), "重排测试应启动服务");
    fixture.timing.ProcessPendingCommands(Trigger(1'000));
    const int64_t first_id = *fixture.repository.FindById(1).value->reminder_task_id;

    Schedule updated = *fixture.repository.FindById(1).value;
    updated.event = "新提醒";
    updated.start_time = At(1'200);
    Check(fixture.repository.Update(updated).ok(), "应保存修改后的日程");
    Check(fixture.reminder.SynchronizeSchedule(1).ok(), "修改时间或文本后应重新同步提醒");
    fixture.timing.ProcessPendingCommands(Trigger(1'001));
    const int64_t second_id = *fixture.repository.FindById(1).value->reminder_task_id;
    Check(second_id != first_id, "重新注册必须使用从未使用过的新 TaskId");
    Check(fixture.timing.RunDueTasks(Trigger(1'100)).processed_count == 0, "旧任务取消后不应在原时间触发");
    Check(fixture.timing.RunDueTasks(Trigger(1'200)).processed_count == 1 &&
              fixture.speech.texts.front() == "提醒：现在是「新提醒」时间了",
          "新任务应在修改后的时间触发并使用新文本");
}

void CheckRecurringFailureStillContinues() {
    Fixture fixture({MakeSchedule(1, "周期首条", At(1'100), 7)});
    fixture.rules.rules.push_back(DailyRule(7));
    fixture.speech.next_status = Status::Error(ErrorCode::kUnavailable, "TTS 不可用");
    Check(fixture.reminder.Start().ok(), "周期测试应启动服务");
    fixture.timing.RunDueTasks(Trigger(1'100));
    Check(fixture.rules.create_next_calls == 1, "周期提醒即使 TTS 失败也必须继续生成下一实例");
    const auto schedules = fixture.repository.FindAll();
    Check(schedules.ok() && schedules.value->size() == 2, "周期回调应保存并同步下一实例");
    const auto& next = schedules.value->back();
    Check(next.rule_id == 7 && next.reminder_task_id.has_value(), "下一实例应关联原规则并注册提醒");
}

void CheckGenerationRetryBackoff() {
    Fixture fixture({MakeSchedule(1, "周期首条", At(1'100), 8)});
    fixture.rules.rules.push_back(DailyRule(8));
    fixture.rules.fail_create_next_count = 3;
    Check(fixture.reminder.Start().ok(), "重试测试应启动服务");

    fixture.now = At(1'100);
    fixture.timing.RunDueTasks(Trigger(1'100));
    Check(fixture.timing.NextWakeAt() == Trigger(1'160), "首次生成失败应约一分钟后重试");
    fixture.now = At(1'160);
    fixture.timing.RunDueTasks(Trigger(1'160));
    Check(fixture.timing.NextWakeAt() == Trigger(1'460), "第二次生成失败应约五分钟后重试");
    fixture.now = At(1'460);
    fixture.timing.RunDueTasks(Trigger(1'460));
    Check(fixture.timing.NextWakeAt() == Trigger(2'360), "第三次生成失败应约十五分钟后重试");
    fixture.now = At(2'360);
    fixture.timing.RunDueTasks(Trigger(2'360));
    Check(fixture.rules.create_next_calls == 4, "第三次之后应继续按封顶间隔尝试而不是静默终止");
}

}  // namespace

int main() {
    CheckFutureMemoAndExpiredRestoration();
    CheckSpeechFailureLeavesActive();
    CheckCancellationAndRescheduleUseFreshIds();
    CheckRecurringFailureStillContinues();
    CheckGenerationRetryBackoff();
    return 0;
}
