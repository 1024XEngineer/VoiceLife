#include "voicelife/schedule/schedule_reminder_service.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
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
using voicelife::timing::CancelTaskCommand;
using voicelife::timing::CancelTaskResult;
using voicelife::timing::CommandAcceptance;
using voicelife::timing::InMemoryTimingTaskRunner;
using voicelife::timing::RegisterTaskCommand;
using voicelife::timing::RegisterTaskResult;
using voicelife::timing::TaskCallback;
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

class ScriptedTimingService final : public voicelife::timing::TimingTaskService {
   public:
    CommandAcceptance RegisterTask(RegisterTaskCommand command) override {
        ++register_calls;
        if (report_register_result && command.on_result) command.on_result(register_result);
        register_commands.push_back(std::move(command));
        return register_acceptance;
    }

    CommandAcceptance CancelTask(CancelTaskCommand command) override {
        ++cancel_calls;
        if (cancel_hook) cancel_hook();
        if (report_cancel_result && command.on_result) command.on_result(cancel_result);
        cancel_commands.push_back(std::move(command));
        return cancel_acceptance;
    }

    CommandAcceptance register_acceptance = CommandAcceptance::kAccepted;
    CommandAcceptance cancel_acceptance = CommandAcceptance::kAccepted;
    bool report_register_result = false;
    bool report_cancel_result = false;
    RegisterTaskResult register_result = RegisterTaskResult::kRegistered;
    CancelTaskResult cancel_result = CancelTaskResult::kCancelled;
    std::function<void()> cancel_hook;
    int register_calls = 0;
    int cancel_calls = 0;
    std::vector<RegisterTaskCommand> register_commands;
    std::vector<CancelTaskCommand> cancel_commands;
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
                      std::optional<ScheduleRuleId> rule_id = std::nullopt,
                      std::optional<int64_t> reminder_task_id = std::nullopt,
                      ScheduleStatus status = ScheduleStatus::kActive) {
    return {
        .id = id,
        .event = std::move(event),
        .start_time = start,
        .end_time = std::nullopt,
        .location = std::nullopt,
        .notes = std::nullopt,
        .rule_id = rule_id,
        .reminder_task_id = reminder_task_id,
        .status = status,
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

struct ScriptedFixture {
    explicit ScriptedFixture(std::vector<Schedule> schedules, DateTime current = At(1'000))
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
    ScriptedTimingService timing;
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

void CheckInvalidAndNotRunningPaths() {
    Fixture fixture({
        MakeSchedule(1, "未来提醒", At(1'100)),
        MakeSchedule(2, "已取消提醒", At(1'150), std::nullopt, std::nullopt, ScheduleStatus::kCancelled),
    });
    fixture.reminder.Stop();
    Check(!fixture.reminder.SynchronizeSchedule(1).ok(), "未启动时不应同步提醒");
    Check(!fixture.reminder.SuspendRuleReminders(0).ok(), "SuspendRuleReminders 应拒绝非法规则 ID");
    Check(!fixture.reminder.SynchronizeRule(0).ok(), "SynchronizeRule 应拒绝非法规则 ID");

    Check(fixture.reminder.Start().ok(), "首次启动应成功");
    Check(fixture.reminder.Start().ok(), "重复启动应保持幂等");
    Check(!fixture.reminder.SynchronizeSchedule(999).ok(), "同步不存在日程应返回仓储错误");
    Check(!fixture.reminder.CancelScheduleReminder(999).ok(), "取消不存在日程应返回仓储错误");
    fixture.timing.ProcessPendingCommands(Trigger(1'000));
    const auto cancelled = fixture.repository.FindById(2);
    Check(cancelled.ok() && !cancelled.value->reminder_task_id.has_value(), "已取消日程启动时不应注册提醒");
}

void CheckCompleteScheduleErrorPaths() {
    Fixture fixture({
        MakeSchedule(1, "可完成提醒", At(1'100), std::nullopt, 42),
        MakeSchedule(2, "已完成提醒", At(1'100), std::nullopt, std::nullopt, ScheduleStatus::kCompleted),
    });

    Check(!fixture.schedule_service.complete_schedule(0).ok(), "完成日程应拒绝非法 ID");
    Check(!fixture.schedule_service.complete_schedule(999).ok(), "完成不存在的日程应返回仓储错误");
    Check(!fixture.schedule_service.complete_schedule(2).ok(), "完成非 Active 日程应返回冲突");
    Check(!fixture.schedule_service.complete_schedule(1, 99).ok(), "提醒任务标识不匹配时应忽略过期回调");

    Check(fixture.schedule_service.complete_schedule(1, 42).ok(), "匹配提醒任务标识时完成日程应成功");
    const auto completed = fixture.repository.FindById(1);
    Check(completed.ok() && completed.value->status == ScheduleStatus::kCompleted &&
              !completed.value->reminder_task_id.has_value(),
          "完成后应更新状态并清空提醒任务标识");
}

void CheckRepositoryFailurePaths() {
    Fixture fixture({MakeSchedule(1, "仓储失败提醒", At(1'100), 7)});
    fixture.repository.FailNextFindAll(Status::Error(ErrorCode::kUnavailable, "FindAll 失败"));
    Check(!fixture.reminder.Start().ok(), "启动读取全部日程失败时应返回错误");

    Check(fixture.reminder.Start().ok(), "失败后再次启动应成功");
    fixture.timing.ProcessPendingCommands(Trigger(1'000));

    fixture.repository.FailNextFindAll(Status::Error(ErrorCode::kUnavailable, "撤销查询失败"));
    Check(!fixture.reminder.SuspendRuleReminders(7).ok(), "撤销规则提醒查询全部日程失败时应返回错误");

    fixture.repository.FailNextFindAll(Status::Error(ErrorCode::kUnavailable, "规则同步查询失败"));
    Check(!fixture.reminder.SynchronizeRule(7).ok(), "同步规则提醒查询全部日程失败时应返回错误");

    fixture.repository.FailNextFindById(Status::Error(ErrorCode::kUnavailable, "单条查询失败"));
    Check(!fixture.reminder.SynchronizeSchedule(1).ok(), "同步单条提醒查询失败时应返回错误");

    fixture.repository.FailNextFindById(Status::Error(ErrorCode::kUnavailable, "取消查询失败"));
    Check(!fixture.reminder.CancelScheduleReminder(1).ok(), "取消单条提醒查询失败时应返回错误");

    fixture.repository.FailNextFindById(Status::Error(ErrorCode::kUnavailable, "回调查询失败"));
    fixture.timing.RunDueTasks(Trigger(1'100));
    Check(fixture.speech.texts.empty(), "提醒回调查询失败时不应提交 TTS");

    Fixture update_fixture({MakeSchedule(2, "注册持久化失败", At(1'200))});
    update_fixture.repository.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "更新失败"));
    Check(!update_fixture.reminder.Start().ok(), "注册提醒持久化失败时应返回错误");

    Fixture clear_fixture({MakeSchedule(3, "清理持久化失败", At(1'200), std::nullopt, 321)});
    clear_fixture.repository.FailNextUpdate(Status::Error(ErrorCode::kUnavailable, "清理失败"));
    Check(!clear_fixture.reminder.Start().ok(), "取消持久化提醒时清理更新失败应返回错误");
}

void CheckSuspendAndSynchronizeRule() {
    Fixture fixture({
        MakeSchedule(1, "规则实例一", At(1'100), 7),
        MakeSchedule(2, "规则实例二", At(1'200), 7),
        MakeSchedule(3, "其他规则", At(1'300), 8),
    });
    fixture.rules.rules.push_back(DailyRule(7));
    fixture.rules.rules.push_back(DailyRule(8));
    Check(fixture.reminder.Start().ok(), "规则同步测试应启动服务");
    fixture.timing.ProcessPendingCommands(Trigger(1'000));

    Check(fixture.reminder.SuspendRuleReminders(7).ok(), "撤销规则提醒应成功");
    fixture.timing.ProcessPendingCommands(Trigger(1'001));
    const auto first = fixture.repository.FindById(1);
    const auto second = fixture.repository.FindById(2);
    const auto other = fixture.repository.FindById(3);
    Check(first.ok() && !first.value->reminder_task_id.has_value(), "规则实例一应清空提醒任务标识");
    Check(second.ok() && !second.value->reminder_task_id.has_value(), "规则实例二应清空提醒任务标识");
    Check(other.ok() && other.value->reminder_task_id.has_value(), "其他规则提醒不应被撤销");

    Check(fixture.reminder.SynchronizeRule(7).ok(), "重新同步规则提醒应成功");
    fixture.timing.ProcessPendingCommands(Trigger(1'002));
    Check(fixture.repository.FindById(1).value->reminder_task_id.has_value() &&
              fixture.repository.FindById(2).value->reminder_task_id.has_value(),
          "规则内未来实例应重新注册提醒");
}

void CheckSuspendRetryTaskAndStopCancelsTasks() {
    Fixture fixture({MakeSchedule(1, "重试实例", At(1'100), 9)});
    fixture.rules.rules.push_back(DailyRule(9));
    fixture.rules.fail_create_next_count = 1;
    Check(fixture.reminder.Start().ok(), "撤销重试测试应启动服务");
    fixture.timing.RunDueTasks(Trigger(1'100));
    Check(fixture.timing.NextWakeAt().has_value(), "生成失败后应存在重试任务");

    Check(fixture.reminder.SuspendRuleReminders(9).ok(), "撤销规则提醒和生成重试应成功");
    fixture.timing.ProcessPendingCommands(Trigger(1'101));
    Check(!fixture.timing.NextWakeAt().has_value(), "规则重试任务和实例提醒都应被撤销");

    Fixture stop_fixture({MakeSchedule(2, "停止提醒", At(1'200))});
    Check(stop_fixture.reminder.Start().ok(), "停止测试应启动服务");
    stop_fixture.timing.ProcessPendingCommands(Trigger(1'000));
    stop_fixture.reminder.Stop();
    stop_fixture.timing.ProcessPendingCommands(Trigger(1'001));
    Check(!stop_fixture.timing.NextWakeAt().has_value(), "Stop 后不应保留待触发提醒");
}

void CheckStopCancelsGenerationRetry() {
    Fixture fixture({MakeSchedule(1, "停止重试", At(1'100), 11)});
    fixture.rules.rules.push_back(DailyRule(11));
    fixture.rules.fail_create_next_count = 1;
    Check(fixture.reminder.Start().ok(), "停止重试测试应启动服务");
    fixture.timing.RunDueTasks(Trigger(1'100));
    Check(fixture.timing.NextWakeAt().has_value(), "生成失败后应有待触发重试任务");

    fixture.reminder.Stop();
    fixture.timing.ProcessPendingCommands(Trigger(1'101));
    Check(!fixture.timing.NextWakeAt().has_value(), "Stop 应取消原提醒和生成重试任务");
}

void CheckAllocationWrapAndInvalidCallback() {
    Fixture wrap_fixture(
        {MakeSchedule(1, "最大任务标识", At(1'100), std::nullopt, std::numeric_limits<int64_t>::max())});
    Check(wrap_fixture.reminder.Start().ok(), "任务标识回绕测试应启动服务");
    wrap_fixture.timing.ProcessPendingCommands(Trigger(1'000));
    const auto wrapped = wrap_fixture.repository.FindById(1);
    Check(wrapped.ok() && wrapped.value->reminder_task_id == 1, "达到最大 TaskId 后应从 1 重新分配");

    ScriptedFixture invalid_fixture({MakeSchedule(2, "非法回调", At(1'100))});
    Check(invalid_fixture.reminder.Start().ok(), "非法回调测试应启动服务");
    const RegisterTaskCommand& registered = invalid_fixture.timing.register_commands.front();
    const auto invalid_task_id = voicelife::timing::TaskId::Create("not-a-number");
    Check(invalid_task_id.has_value(), "非数字任务标识仍可创建");
    registered.callback(*invalid_task_id, Trigger(1'100));
    Check(invalid_fixture.speech.texts.empty(), "无法解析的任务回调不应触发 TTS");
}

void CheckTimingFailureAndDuplicatePaths() {
    ScriptedFixture fixture({MakeSchedule(1, "未来提醒", At(1'100))});
    fixture.timing.register_acceptance = CommandAcceptance::kUnavailable;
    Check(!fixture.reminder.Start().ok(), "注册命令不可用时启动应返回错误");
    const auto unavailable_register = fixture.repository.FindById(1);
    Check(unavailable_register.ok() && !unavailable_register.value->reminder_task_id.has_value(),
          "注册命令不可用时应回滚持久化提醒任务标识");

    ScriptedFixture duplicate_fixture({MakeSchedule(2, "重复提醒", At(1'100))});
    duplicate_fixture.timing.report_register_result = true;
    duplicate_fixture.timing.register_result = RegisterTaskResult::kDuplicate;
    Check(duplicate_fixture.reminder.Start().ok(), "重复注册结果不应使启动失败");
    const auto duplicate = duplicate_fixture.repository.FindById(2);
    Check(duplicate.ok() && !duplicate.value->reminder_task_id.has_value(), "注册结果重复时应清空持久化提醒任务标识");

    ScriptedFixture cancel_fixture({MakeSchedule(3, "取消失败", At(1'100))});
    cancel_fixture.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    cancel_fixture.repository.Update([&] {
        Schedule schedule = *cancel_fixture.repository.FindById(3).value;
        schedule.reminder_task_id = 321;
        return schedule;
    }());
    Check(!cancel_fixture.reminder.Start().ok(), "取消命令不可用时启动应返回错误");
    const auto failed_cancel = cancel_fixture.repository.FindById(3);
    Check(failed_cancel.ok() && failed_cancel.value->reminder_task_id == 321, "取消命令不可用时应保留原有提醒任务标识");

    ScriptedFixture no_task_fixture({MakeSchedule(4, "无提醒任务", At(1'100))});
    Check(no_task_fixture.reminder.CancelScheduleReminder(4).ok(), "无提醒任务标识时取消应幂等成功");
    Check(no_task_fixture.timing.cancel_calls == 0, "无提醒任务标识时不应提交取消命令");
}

void CheckStaleReminderCallbackIsIgnored() {
    ScriptedFixture fixture({MakeSchedule(1, "回调提醒", At(1'100))});
    Check(fixture.reminder.Start().ok(), "回调测试应启动服务");
    Check(fixture.timing.register_commands.size() == 1, "启动应提交一个提醒注册命令");

    const RegisterTaskCommand& registered = fixture.timing.register_commands.front();
    const auto task_id = voicelife::timing::TaskId::Create(registered.task_id.Value());
    Check(task_id.has_value(), "注册命令应包含有效 TaskId");
    const int64_t first_task_id = *fixture.repository.FindById(1).value->reminder_task_id;
    Check(first_task_id > 0, "启动后应持久化提醒任务标识");

    Schedule updated = *fixture.repository.FindById(1).value;
    updated.reminder_task_id = first_task_id + 1;
    Check(fixture.repository.Update(updated).ok(), "应模拟提醒任务已被替换");
    registered.callback(*task_id, Trigger(1'100));
    Check(fixture.speech.texts.empty(), "过期提醒回调不应触发 TTS");

    fixture.reminder.Stop();
    registered.callback(*task_id, Trigger(1'100));
    Check(fixture.speech.texts.empty(), "服务停止后的回调不应触发 TTS");
}

void CheckGenerationRetryUnavailableAndStopCancelsRetry() {
    ScriptedFixture fixture({MakeSchedule(1, "重试失败实例", At(1'100), 10)});
    fixture.rules.rules.push_back(DailyRule(10));
    fixture.rules.fail_create_next_count = 1;
    Check(fixture.reminder.Start().ok(), "重试不可用测试应启动服务");
    Check(fixture.timing.register_commands.size() == 1, "启动应先注册原实例提醒");

    const RegisterTaskCommand& first = fixture.timing.register_commands.front();
    const auto first_task_id = voicelife::timing::TaskId::Create(first.task_id.Value());
    fixture.timing.register_acceptance = CommandAcceptance::kUnavailable;
    first.callback(*first_task_id, Trigger(1'100));
    Check(fixture.rules.create_next_calls == 1 && fixture.timing.register_calls == 2, "生成失败后应尝试注册重试任务");

    fixture.reminder.Stop();
    Check(fixture.timing.cancel_calls == 0, "原实例完成后且重试注册不可用时 Stop 不应提交无效取消");
}

void CheckGenerationRetryDuplicateAndStaleCallbacks() {
    ScriptedFixture fixture({MakeSchedule(1, "重复重试实例", At(1'100), 12)});
    fixture.rules.rules.push_back(DailyRule(12));
    fixture.rules.fail_create_next_count = 1;
    Check(fixture.reminder.Start().ok(), "重复重试测试应启动服务");

    const RegisterTaskCommand& first = fixture.timing.register_commands.front();
    const auto first_task_id = voicelife::timing::TaskId::Create(first.task_id.Value());
    fixture.timing.report_register_result = true;
    fixture.timing.register_result = RegisterTaskResult::kDuplicate;
    first.callback(*first_task_id, Trigger(1'100));
    Check(fixture.timing.register_calls == 2, "生成失败后应尝试注册重试任务");

    const RegisterTaskCommand& retry = fixture.timing.register_commands.back();
    const auto retry_task_id = voicelife::timing::TaskId::Create(retry.task_id.Value());
    retry.callback(*retry_task_id, Trigger(1'160));
    Check(fixture.rules.create_next_calls == 1, "重试注册结果为重复时后续回调应被忽略");

    fixture.reminder.Stop();
    Check(fixture.timing.cancel_calls == 0, "重试注册已被标记重复时 Stop 不应提交重试取消");
}

void CheckSuspendRetryTaskUnavailable() {
    ScriptedFixture fixture({MakeSchedule(1, "撤销失败实例", At(1'100), 13)});
    fixture.rules.rules.push_back(DailyRule(13));
    fixture.rules.fail_create_next_count = 1;
    Check(fixture.reminder.Start().ok(), "撤销重试不可用测试应启动服务");

    const RegisterTaskCommand& first = fixture.timing.register_commands.front();
    const auto first_task_id = voicelife::timing::TaskId::Create(first.task_id.Value());
    first.callback(*first_task_id, Trigger(1'100));
    Check(fixture.timing.register_calls == 2, "生成失败后应有重试注册命令");

    fixture.timing.cancel_acceptance = CommandAcceptance::kUnavailable;
    Check(!fixture.reminder.SuspendRuleReminders(13).ok(), "撤销重试取消命令不可用时应返回错误");
}

}  // namespace

int main() {
    CheckFutureMemoAndExpiredRestoration();
    CheckSpeechFailureLeavesActive();
    CheckCancellationAndRescheduleUseFreshIds();
    CheckRecurringFailureStillContinues();
    CheckGenerationRetryBackoff();
    CheckInvalidAndNotRunningPaths();
    CheckCompleteScheduleErrorPaths();
    CheckRepositoryFailurePaths();
    CheckSuspendAndSynchronizeRule();
    CheckSuspendRetryTaskAndStopCancelsTasks();
    CheckStopCancelsGenerationRetry();
    CheckAllocationWrapAndInvalidCallback();
    CheckTimingFailureAndDuplicatePaths();
    CheckStaleReminderCallbackIsIgnored();
    CheckGenerationRetryUnavailableAndStopCancelsRetry();
    CheckGenerationRetryDuplicateAndStaleCallbacks();
    CheckSuspendRetryTaskUnavailable();
    return 0;
}
