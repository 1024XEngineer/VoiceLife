#include "voicelife/schedule/schedule_reminder_service.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "voicelife/timing/timing_task.h"

namespace voicelife::schedule {
namespace {
using namespace std::chrono_literals;
constexpr int kMaximumAttempts = 3;
constexpr auto kFollowUpDelay = 10min;
constexpr auto kRecentWindow = 10min;
constexpr std::string_view kFinalSnoozeReminderNotice = "这是最后一次提醒；之后不再创建新的推迟提醒。";
DateTime SystemNow() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }
timing::TriggerAt ToTriggerAt(DateTime value) { return std::chrono::time_point_cast<std::chrono::microseconds>(value); }
std::chrono::minutes RetryDelay(int failures) { return failures <= 1 ? 1min : failures == 2 ? 5min : 15min; }
bool Pending(const ScheduleReminderTask& task) { return task.timer_status == ScheduleReminderTimerStatus::kPending; }
}  // namespace

ScheduleReminderService::ScheduleReminderService(
    ScheduleRepository& repository, ScheduleReminderTaskRepository& reminder_repository,
    ScheduleService& schedule_service, ScheduleRuleService& rule_service, timing::TimingTaskService& timing_service,
    ScheduleReminderSpeechPort& speech, ScheduleReminderNotificationPort* notification, NowProvider now_provider)
    : repository_(repository),
      reminder_repository_(reminder_repository),
      schedule_service_(schedule_service),
      rule_service_(rule_service),
      timing_service_(&timing_service),
      speech_(speech),
      notification_(notification),
      now_provider_(now_provider ? std::move(now_provider) : NowProvider{SystemNow}) {}

Status ScheduleReminderService::Start() {
    const auto reminders = reminder_repository_.FindAll();
    if (!reminders.ok()) return reminders.status;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return Status::Ok();
        running_ = true;
        sequence_ =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        for (const auto& task : *reminders.value) {
            chain_sequence_ = std::max(chain_sequence_, task.chain_id);
        }
    }

    Status first_failure = Status::Ok();
    std::unordered_set<ScheduleId> schedules_with_tasks;
    for (const auto& task : *reminders.value) {
        schedules_with_tasks.insert(task.schedule_id);
        if (task.timer_status != ScheduleReminderTimerStatus::kPending) continue;
        const auto schedule = repository_.FindById(task.schedule_id);
        if (!schedule.ok() || schedule.value->status != ScheduleStatus::kActive) {
            ScheduleReminderTask cancelled = task;
            cancelled.timer_status = ScheduleReminderTimerStatus::kCancelled;
            cancelled.business_status = ScheduleReminderBusinessStatus::kCancelled;
            cancelled.updated_at = Now();
            (void)reminder_repository_.Update(cancelled);
            continue;
        }
        if (task.trigger_at <= Now()) {
            HandleReminder(task.id, task.timing_task_id.value_or(""));
            continue;
        }
        const Status restored = RegisterPersistedTask(task);
        if (!restored.ok() && first_failure.ok()) first_failure = restored;
    }

    const auto schedules = repository_.FindAll();
    if (!schedules.ok()) return schedules.status;
    for (const auto& schedule : *schedules.value) {
        if (schedule.status != ScheduleStatus::kActive || !schedule.start_time.has_value() ||
            *schedule.start_time <= Now() || schedules_with_tasks.contains(schedule.id))
            continue;
        const Status status = RegisterReminder(schedule.id, AllocateChainId(), 1, *schedule.start_time);
        if (!status.ok() && first_failure.ok()) first_failure = status;
    }
    return first_failure;
}

void ScheduleReminderService::Stop() {
    std::vector<std::string> ids;
    const auto all = reminder_repository_.FindAll();
    if (all.ok())
        for (const auto& task : *all.value)
            if (Pending(task) && task.timing_task_id) ids.push_back(*task.timing_task_id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        for (const auto& [rule, retry] : generation_retries_) {
            (void)rule;
            ids.push_back(retry.task_id);
        }
        generation_retries_.clear();
    }
    for (const auto& id : ids) {
        const auto task_id = timing::TaskId::Create(id);
        if (task_id) (void)timing_service_->CancelTask({.task_id = *task_id, .on_result = {}});
    }
}

Status ScheduleReminderService::SynchronizeSchedule(ScheduleId schedule_id) {
    if (!IsRunning()) return Status::Error(ErrorCode::kUnavailable, "日程提醒服务尚未启动");
    const auto loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;
    const Status cancelled = CancelPendingTasks(schedule_id);
    if (!cancelled.ok()) return cancelled;
    if (loaded.value->status != ScheduleStatus::kActive || !loaded.value->start_time.has_value() ||
        *loaded.value->start_time <= Now())
        return Status::Ok();
    return RegisterReminder(schedule_id, AllocateChainId(), 1, *loaded.value->start_time);
}

Status ScheduleReminderService::CancelScheduleReminder(ScheduleId schedule_id) {
    const auto loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;
    return CancelPendingTasks(schedule_id);
}

Status ScheduleReminderService::CancelPendingTasks(ScheduleId schedule_id, std::optional<int64_t> except_task_id) {
    const auto tasks = reminder_repository_.FindBySchedule(schedule_id);
    if (!tasks.ok()) return tasks.status;
    Status first_failure = Status::Ok();
    for (auto task : *tasks.value) {
        if (!Pending(task) || task.id == except_task_id) continue;
        if (task.timing_task_id) {
            const auto id = timing::TaskId::Create(*task.timing_task_id);
            if (id && timing_service_->CancelTask({.task_id = *id, .on_result = {}}) ==
                          timing::CommandAcceptance::kUnavailable) {
                if (first_failure.ok())
                    first_failure = Status::Error(ErrorCode::kUnavailable, "提醒任务取消命令未被接收");
                continue;
            }
        }
        task.timer_status = ScheduleReminderTimerStatus::kCancelled;
        task.business_status = ScheduleReminderBusinessStatus::kCancelled;
        task.updated_at = Now();
        const Status updated = reminder_repository_.Update(task);
        if (!updated.ok() && first_failure.ok()) first_failure = updated;
    }
    return first_failure;
}

Status ScheduleReminderService::SuspendRuleReminders(ScheduleRuleId rule_id) {
    if (rule_id <= 0) return Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零");
    const auto schedules = repository_.FindAll();
    if (!schedules.ok()) return schedules.status;
    Status first_failure = Status::Ok();
    for (const auto& schedule : *schedules.value) {
        if (schedule.rule_id != rule_id) continue;
        const Status status = CancelPendingTasks(schedule.id);
        if (!status.ok() && first_failure.ok()) first_failure = status;
    }
    std::optional<std::string> retry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = generation_retries_.find(rule_id);
        if (found != generation_retries_.end()) {
            retry = found->second.task_id;
            generation_retries_.erase(found);
        }
    }
    if (retry) {
        const auto id = timing::TaskId::Create(*retry);
        if (id &&
            timing_service_->CancelTask({.task_id = *id, .on_result = {}}) == timing::CommandAcceptance::kUnavailable &&
            first_failure.ok())
            first_failure = Status::Error(ErrorCode::kUnavailable, "周期生成重试取消命令未被接收");
    }
    return first_failure;
}

Status ScheduleReminderService::SynchronizeRule(ScheduleRuleId rule_id) {
    if (rule_id <= 0) return Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零");
    const auto schedules = repository_.FindAll();
    if (!schedules.ok()) return schedules.status;
    Status first_failure = Status::Ok();
    for (const auto& schedule : *schedules.value) {
        if (schedule.rule_id != rule_id || schedule.status != ScheduleStatus::kActive) continue;
        const Status status = SynchronizeSchedule(schedule.id);
        if (!status.ok() && first_failure.ok()) first_failure = status;
    }
    return first_failure;
}

Result<ReminderActionResult> ScheduleReminderService::AcknowledgeRecentReminders() {
    std::lock_guard<std::mutex> action_lock(action_mutex_);
    const auto recent = reminder_repository_.FindTriggered(Now() - kRecentWindow, Now());
    if (!recent.ok()) return Result<ReminderActionResult>::Failure(recent.status.code, recent.status.message);
    std::unordered_set<int64_t> chains;
    std::unordered_set<ScheduleId> schedules;
    std::vector<std::string> events;
    std::unordered_set<std::string> event_set;
    for (const auto& task : *recent.value) {
        if (task.business_status != ScheduleReminderBusinessStatus::kWaitingAcknowledgement &&
            task.business_status != ScheduleReminderBusinessStatus::kExhausted)
            continue;
        chains.insert(task.chain_id);
        schedules.insert(task.schedule_id);
        std::string event = task.event;
        if (event.empty()) {
            const auto loaded = repository_.FindById(task.schedule_id);
            if (loaded.ok()) event = loaded.value->event;
        }
        if (!event.empty() && event_set.insert(event).second) events.push_back(std::move(event));
    }
    if (chains.empty())
        return Result<ReminderActionResult>::Failure(ErrorCode::kNotFound, "最近 10 分钟内没有已触发的提醒");
    const auto all = reminder_repository_.FindAll();
    if (!all.ok()) return Result<ReminderActionResult>::Failure(all.status.code, all.status.message);
    for (auto task : *all.value) {
        if (!chains.contains(task.chain_id)) continue;
        if (Pending(task) && task.timing_task_id) {
            const auto id = timing::TaskId::Create(*task.timing_task_id);
            if (id && timing_service_->CancelTask({.task_id = *id, .on_result = {}}) ==
                          timing::CommandAcceptance::kUnavailable)
                return Result<ReminderActionResult>::Failure(ErrorCode::kUnavailable, "后续提醒取消命令未被接收");
            task.timer_status = ScheduleReminderTimerStatus::kCancelled;
        }
        if (task.business_status == ScheduleReminderBusinessStatus::kScheduled ||
            task.business_status == ScheduleReminderBusinessStatus::kWaitingAcknowledgement ||
            task.business_status == ScheduleReminderBusinessStatus::kExhausted) {
            task.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
        }
        task.updated_at = Now();
        const Status status = reminder_repository_.Update(task);
        if (!status.ok()) return Result<ReminderActionResult>::Failure(status.code, status.message);
    }
    for (ScheduleId id : schedules) {
        const auto loaded = repository_.FindById(id);
        if (loaded.ok() && loaded.value->status == ScheduleStatus::kActive) {
            const Status completed = schedule_service_.complete_schedule(id);
            if (!completed.ok()) return Result<ReminderActionResult>::Failure(completed.code, completed.message);
        }
    }
    return Result<ReminderActionResult>::Success({.affected_count = static_cast<int>(chains.size()),
                                                  .events = std::move(events),
                                                  .operation_id = {},
                                                  .reminder_trigger_id = {},
                                                  .action = ScheduleReminderActionKind::kAcknowledge,
                                                  .occurred_at = {},
                                                  .next_trigger_at = std::nullopt,
                                                  .replayed = false});
}

Result<ReminderActionResult> ScheduleReminderService::SnoozeRecentReminders() {
    std::lock_guard<std::mutex> action_lock(action_mutex_);
    const auto recent = reminder_repository_.FindTriggered(Now() - kRecentWindow, Now());
    if (!recent.ok()) return Result<ReminderActionResult>::Failure(recent.status.code, recent.status.message);
    std::unordered_set<int64_t> chains;
    const auto all = reminder_repository_.FindAll();
    if (!all.ok()) return Result<ReminderActionResult>::Failure(all.status.code, all.status.message);
    for (const auto& triggered : *recent.value) {
        if (triggered.business_status != ScheduleReminderBusinessStatus::kWaitingAcknowledgement &&
            triggered.business_status != ScheduleReminderBusinessStatus::kExhausted)
            continue;
        const bool has_pending_follow_up =
            std::any_of(all.value->begin(), all.value->end(), [&triggered](const auto& task) {
                return task.chain_id == triggered.chain_id &&
                       task.timer_status == ScheduleReminderTimerStatus::kPending &&
                       task.business_status == ScheduleReminderBusinessStatus::kScheduled;
            });
        if (has_pending_follow_up) chains.insert(triggered.chain_id);
    }
    if (chains.empty())
        return Result<ReminderActionResult>::Failure(ErrorCode::kNotFound, "最近 10 分钟内没有可延迟的提醒");
    return Result<ReminderActionResult>::Success({.affected_count = static_cast<int>(chains.size()),
                                                  .events = {},
                                                  .operation_id = {},
                                                  .reminder_trigger_id = {},
                                                  .action = ScheduleReminderActionKind::kSnooze,
                                                  .occurred_at = {},
                                                  .next_trigger_at = std::nullopt,
                                                  .replayed = false});
}

Result<ReminderActionResult> ScheduleReminderService::ExecuteReminderAction(const ReminderActionCommand& command) {
    if (command.operation_id.empty() || command.reminder_trigger_id.empty()) {
        return Result<ReminderActionResult>::Failure(ErrorCode::kInvalidArgument, "提醒动作标识不能为空");
    }
    if (command.action != ScheduleReminderActionKind::kAcknowledge &&
        command.action != ScheduleReminderActionKind::kSnooze) {
        return Result<ReminderActionResult>::Failure(ErrorCode::kInvalidArgument, "提醒动作类型无效");
    }
    const bool acknowledge = command.action == ScheduleReminderActionKind::kAcknowledge;
    if ((acknowledge && command.snooze_minutes.has_value()) ||
        (!acknowledge && command.snooze_minutes != static_cast<int>(kFollowUpDelay.count()))) {
        return Result<ReminderActionResult>::Failure(ErrorCode::kInvalidArgument, "提醒动作参数无效");
    }

    std::lock_guard<std::mutex> action_lock(action_mutex_);
    auto target_result = reminder_repository_.FindByTimingTaskId(command.reminder_trigger_id);
    if (!target_result.ok()) {
        return Result<ReminderActionResult>::Failure(target_result.status.code, target_result.status.message);
    }
    ScheduleReminderTask target = *target_result.value;
    if (target.action_operation_id.has_value()) {
        // Acknowledge/snooze may arrive through both voice MCP and the IM card.
        // Treat a same-kind action as an idempotent replay even when the
        // transport assigned a different operationId; a different action kind
        // remains a conflict and must not mutate the completed reminder.
        if (target.action_kind != command.action || !target.action_occurred_at.has_value()) {
            return Result<ReminderActionResult>::Failure(ErrorCode::kAlreadyExists,
                                                         "提醒已由其他 operationId 或动作处理");
        }
        return Result<ReminderActionResult>::Success({.affected_count = 1,
                                                      .events = {},
                                                      .operation_id = *target.action_operation_id,
                                                      .reminder_trigger_id = command.reminder_trigger_id,
                                                      .action = *target.action_kind,
                                                      .occurred_at = *target.action_occurred_at,
                                                      .next_trigger_at = target.action_next_trigger_at,
                                                      .replayed = true});
    }
    if (target.business_status != ScheduleReminderBusinessStatus::kWaitingAcknowledgement &&
        target.business_status != ScheduleReminderBusinessStatus::kExhausted) {
        return Result<ReminderActionResult>::Failure(ErrorCode::kAlreadyExists, "提醒已经进入不可操作终态");
    }

    const auto all_tasks = reminder_repository_.FindAll();
    if (!all_tasks.ok()) {
        return Result<ReminderActionResult>::Failure(all_tasks.status.code, all_tasks.status.message);
    }
    const auto conflicting_operation =
        std::find_if(all_tasks.value->begin(), all_tasks.value->end(),
                     [&command](const auto& task) { return task.action_operation_id == command.operation_id; });
    if (conflicting_operation != all_tasks.value->end()) {
        return Result<ReminderActionResult>::Failure(ErrorCode::kAlreadyExists, "operationId 已用于其他提醒动作");
    }

    const auto schedule_tasks = reminder_repository_.FindBySchedule(target.schedule_id);
    if (!schedule_tasks.ok()) {
        return Result<ReminderActionResult>::Failure(schedule_tasks.status.code, schedule_tasks.status.message);
    }
    const DateTime occurred_at = Now();
    if (!acknowledge) {
        const auto follow_up =
            std::find_if(schedule_tasks.value->begin(), schedule_tasks.value->end(), [&target](const auto& task) {
                return task.chain_id == target.chain_id && task.attempt > target.attempt &&
                       task.business_status == ScheduleReminderBusinessStatus::kScheduled &&
                       task.timer_status == ScheduleReminderTimerStatus::kPending;
            });
        if (follow_up == schedule_tasks.value->end()) {
            return Result<ReminderActionResult>::Failure(ErrorCode::kNotFound, "提醒没有可用的后续触发时间");
        }
        target.business_status = ScheduleReminderBusinessStatus::kSnoozed;
        target.action_operation_id = command.operation_id;
        target.action_kind = command.action;
        target.action_occurred_at = occurred_at;
        target.action_next_trigger_at = follow_up->trigger_at;
        target.updated_at = occurred_at;
        const Status updated = reminder_repository_.Update(target);
        if (!updated.ok()) return Result<ReminderActionResult>::Failure(updated.code, updated.message);
        return Result<ReminderActionResult>::Success({.affected_count = 1,
                                                      .events = {},
                                                      .operation_id = command.operation_id,
                                                      .reminder_trigger_id = command.reminder_trigger_id,
                                                      .action = command.action,
                                                      .occurred_at = occurred_at,
                                                      .next_trigger_at = follow_up->trigger_at,
                                                      .replayed = false});
    }

    for (const auto& task : *schedule_tasks.value) {
        if (task.chain_id != target.chain_id || !Pending(task) || !task.timing_task_id.has_value()) continue;
        const auto task_id = timing::TaskId::Create(*task.timing_task_id);
        if (task_id && timing_service_->CancelTask({.task_id = *task_id, .on_result = {}}) ==
                           timing::CommandAcceptance::kUnavailable) {
            return Result<ReminderActionResult>::Failure(ErrorCode::kUnavailable, "后续提醒取消命令未被接收");
        }
    }
    for (auto task : *schedule_tasks.value) {
        if (task.chain_id != target.chain_id || task.id == target.id) continue;
        if (Pending(task)) task.timer_status = ScheduleReminderTimerStatus::kCancelled;
        if (task.business_status == ScheduleReminderBusinessStatus::kScheduled ||
            task.business_status == ScheduleReminderBusinessStatus::kWaitingAcknowledgement ||
            task.business_status == ScheduleReminderBusinessStatus::kExhausted) {
            task.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
        }
        task.updated_at = occurred_at;
        const Status updated = reminder_repository_.Update(task);
        if (!updated.ok()) return Result<ReminderActionResult>::Failure(updated.code, updated.message);
    }
    const auto schedule = repository_.FindById(target.schedule_id);
    if (!schedule.ok()) return Result<ReminderActionResult>::Failure(schedule.status.code, schedule.status.message);
    if (schedule.value->status == ScheduleStatus::kActive) {
        const Status completed = schedule_service_.complete_schedule(target.schedule_id);
        if (!completed.ok()) return Result<ReminderActionResult>::Failure(completed.code, completed.message);
    } else if (schedule.value->status != ScheduleStatus::kCompleted) {
        return Result<ReminderActionResult>::Failure(ErrorCode::kAlreadyExists, "提醒关联日程不可完成");
    }
    target.business_status = ScheduleReminderBusinessStatus::kAcknowledged;
    target.action_operation_id = command.operation_id;
    target.action_kind = command.action;
    target.action_occurred_at = occurred_at;
    target.action_next_trigger_at = std::nullopt;
    target.updated_at = occurred_at;
    const Status updated = reminder_repository_.Update(target);
    if (!updated.ok()) return Result<ReminderActionResult>::Failure(updated.code, updated.message);
    return Result<ReminderActionResult>::Success({.affected_count = 1,
                                                  .events = {},
                                                  .operation_id = command.operation_id,
                                                  .reminder_trigger_id = command.reminder_trigger_id,
                                                  .action = command.action,
                                                  .occurred_at = occurred_at,
                                                  .next_trigger_at = std::nullopt,
                                                  .replayed = false});
}

Result<std::vector<ReminderActionResult>> ScheduleReminderService::ExecuteRecentReminderActions(
    ScheduleReminderActionKind action) {
    const auto recent = reminder_repository_.FindTriggered(Now() - kRecentWindow, Now());
    if (!recent.ok())
        return Result<std::vector<ReminderActionResult>>::Failure(recent.status.code, recent.status.message);
    std::vector<ReminderActionResult> results;
    Status first_failure = Status::Ok();
    for (const auto& task : *recent.value) {
        if ((task.business_status != ScheduleReminderBusinessStatus::kWaitingAcknowledgement &&
             task.business_status != ScheduleReminderBusinessStatus::kExhausted) ||
            !task.timing_task_id.has_value())
            continue;
        ReminderActionCommand command;
        command.operation_id = AllocateTaskId("voice-action");
        command.reminder_trigger_id = *task.timing_task_id;
        command.action = action;
        if (action == ScheduleReminderActionKind::kSnooze) command.snooze_minutes = 10;
        const auto result = ExecuteReminderAction(command);
        if (result.ok()) {
            ReminderActionResult action_result = *result.value;
            if (action == ScheduleReminderActionKind::kAcknowledge) {
                const auto schedule = repository_.FindById(task.schedule_id);
                if (schedule.ok() && !schedule.value->event.empty())
                    action_result.events.push_back(schedule.value->event);
            }
            results.push_back(std::move(action_result));
        } else if (first_failure.ok()) {
            first_failure = result.status;
        }
    }
    if (!first_failure.ok())
        return Result<std::vector<ReminderActionResult>>::Failure(first_failure.code, first_failure.message);
    if (results.empty()) {
        return Result<std::vector<ReminderActionResult>>::Failure(ErrorCode::kNotFound,
                                                                  "最近 10 分钟内没有可操作的提醒");
    }
    return Result<std::vector<ReminderActionResult>>::Success(std::move(results));
}

Result<std::vector<ReminderActionResult>> ScheduleReminderService::ListPersistedVoiceActionResults() const {
    const auto all = reminder_repository_.FindAll();
    if (!all.ok()) return Result<std::vector<ReminderActionResult>>::Failure(all.status.code, all.status.message);
    std::vector<ReminderActionResult> results;
    for (const auto& task : *all.value) {
        if (!task.action_operation_id.has_value() || task.action_operation_id->rfind("voice-action-", 0) != 0 ||
            !task.action_kind.has_value() || !task.action_occurred_at.has_value() || !task.timing_task_id.has_value())
            continue;
        results.push_back({.affected_count = 1,
                           .events = {},
                           .operation_id = *task.action_operation_id,
                           .reminder_trigger_id = *task.timing_task_id,
                           .action = *task.action_kind,
                           .occurred_at = *task.action_occurred_at,
                           .next_trigger_at = task.action_next_trigger_at,
                           .replayed = true});
    }
    return Result<std::vector<ReminderActionResult>>::Success(std::move(results));
}

DateTime ScheduleReminderService::Now() const { return now_provider_(); }

int64_t ScheduleReminderService::AllocateChainId() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chain_sequence_ == std::numeric_limits<int64_t>::max())
        chain_sequence_ = 1;
    else
        ++chain_sequence_;
    return chain_sequence_;
}

std::string ScheduleReminderService::AllocateTaskId(std::string_view prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sequence_ == std::numeric_limits<int64_t>::max())
        sequence_ = 1;
    else
        ++sequence_;
    return std::string(prefix) + "-" + std::to_string(sequence_);
}

Status ScheduleReminderService::RegisterReminder(ScheduleId schedule_id, int64_t chain_id, int attempt,
                                                 DateTime trigger_at) {
    const auto loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;
    const DateTime now = Now();
    ScheduleReminderTask task{.schedule_id = schedule_id,
                              .event = loaded.value->event,
                              .chain_id = chain_id,
                              .attempt = attempt,
                              .timing_task_id = AllocateTaskId("schedule-reminder"),
                              .trigger_at = trigger_at,
                              .business_status = ScheduleReminderBusinessStatus::kScheduled,
                              .timer_status = ScheduleReminderTimerStatus::kPending,
                              .triggered_at = std::nullopt,
                              .action_operation_id = std::nullopt,
                              .action_kind = std::nullopt,
                              .action_occurred_at = std::nullopt,
                              .action_next_trigger_at = std::nullopt,
                              .created_at = now,
                              .updated_at = now};
    const auto inserted = reminder_repository_.Insert(task);
    if (!inserted.ok()) return inserted.status;
    const Status registered = RegisterPersistedTask(*inserted.value);
    if (!registered.ok()) {
        task = *inserted.value;
        task.timer_status = ScheduleReminderTimerStatus::kFailed;
        task.updated_at = Now();
        (void)reminder_repository_.Update(task);
    }
    return registered;
}

Status ScheduleReminderService::RegisterPersistedTask(const ScheduleReminderTask& task) {
    if (!task.timing_task_id) return Status::Error(ErrorCode::kInternal, "提醒任务缺少 Timing task 标识");
    const auto id = timing::TaskId::Create(*task.timing_task_id);
    if (!id) return Status::Error(ErrorCode::kInternal, "提醒任务标识无效");
    const auto accepted = timing_service_->RegisterTask(
        {.task_id = *id,
         .trigger_at = ToTriggerAt(task.trigger_at),
         .callback = [this, reminder_task_id = task.id](
                         const timing::TaskId& fired,
                         timing::TriggerAt) { HandleReminder(reminder_task_id, fired.Value()); },
         .on_result =
             [this, reminder_task_id = task.id](timing::RegisterTaskResult result) {
                 if (result != timing::RegisterTaskResult::kDuplicate) return;
                 auto loaded = reminder_repository_.FindById(reminder_task_id);
                 if (loaded.ok() && loaded.value->timer_status == ScheduleReminderTimerStatus::kPending) {
                     auto failed = *loaded.value;
                     failed.timer_status = ScheduleReminderTimerStatus::kFailed;
                     failed.updated_at = Now();
                     (void)reminder_repository_.Update(failed);
                 }
             }});
    return accepted == timing::CommandAcceptance::kAccepted
               ? Status::Ok()
               : Status::Error(ErrorCode::kUnavailable, "提醒任务注册命令未被接收");
}

void ScheduleReminderService::HandleReminder(int64_t reminder_task_id, std::string_view timing_task_id) {
    if (!IsRunning()) return;
    const auto loaded_task = reminder_repository_.FindById(reminder_task_id);
    if (!loaded_task.ok() || !Pending(*loaded_task.value) || !loaded_task.value->timing_task_id ||
        *loaded_task.value->timing_task_id != timing_task_id)
        return;
    const auto loaded_schedule = repository_.FindById(loaded_task.value->schedule_id);
    if (!loaded_schedule.ok() || loaded_schedule.value->status != ScheduleStatus::kActive) return;
    ScheduleReminderTask task = *loaded_task.value;
    task.timer_status = ScheduleReminderTimerStatus::kTriggered;
    task.business_status = task.attempt >= kMaximumAttempts ? ScheduleReminderBusinessStatus::kExhausted
                                                            : ScheduleReminderBusinessStatus::kWaitingAcknowledgement;
    task.triggered_at = Now();
    task.updated_at = Now();
    if (!reminder_repository_.Update(task).ok()) return;
    Status follow_up_status = Status::Ok();
    if (task.attempt < kMaximumAttempts) {
        follow_up_status = RegisterReminder(task.schedule_id, task.chain_id, task.attempt + 1, Now() + kFollowUpDelay);
    }
    const std::string text = "提醒：现在是「" + loaded_schedule.value->event + "」时间了" +
                             (task.attempt >= kMaximumAttempts ? " " + std::string(kFinalSnoozeReminderNotice) : "");
    (void)speech_.SpeakScheduleReminder(text);
    if (notification_ && follow_up_status.ok()) (void)notification_->SendScheduleReminder(*loaded_schedule.value, task);
    if (task.attempt == 1 && loaded_schedule.value->rule_id) GenerateNextInstance(*loaded_schedule.value->rule_id, 0);
}

void ScheduleReminderService::GenerateNextInstance(ScheduleRuleId rule_id, int prior_failure_count) {
    if (!IsRunning()) return;
    const auto result = rule_service_.generate_next_schedule_instance({.rule_id = rule_id});
    if (!result.status.ok()) {
        (void)ScheduleGenerationRetry(rule_id, prior_failure_count + 1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_retries_.erase(rule_id);
    }
    if (result.schedule) (void)SynchronizeSchedule(result.schedule->id);
}

Status ScheduleReminderService::ScheduleGenerationRetry(ScheduleRuleId rule_id, int failures) {
    const std::string value = AllocateTaskId("schedule-rule-retry");
    const auto id = timing::TaskId::Create(value);
    if (!id) return Status::Error(ErrorCode::kInternal, "无法创建周期生成重试任务标识");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_retries_[rule_id] = {.task_id = value, .failure_count = failures};
    }
    const auto accepted = timing_service_->RegisterTask(
        {.task_id = *id,
         .trigger_at = ToTriggerAt(Now() + RetryDelay(failures)),
         .callback =
             [this, rule_id, value](const timing::TaskId& fired, timing::TriggerAt) {
                 if (fired.Value() != value) return;
                 int failures = 0;
                 {
                     std::lock_guard<std::mutex> lock(mutex_);
                     auto found = generation_retries_.find(rule_id);
                     if (found == generation_retries_.end() || found->second.task_id != value) return;
                     failures = found->second.failure_count;
                     generation_retries_.erase(found);
                 }
                 GenerateNextInstance(rule_id, failures);
             },
         .on_result =
             [this, rule_id, value](timing::RegisterTaskResult result) {
                 if (result != timing::RegisterTaskResult::kDuplicate) return;
                 std::lock_guard<std::mutex> lock(mutex_);
                 auto found = generation_retries_.find(rule_id);
                 if (found != generation_retries_.end() && found->second.task_id == value)
                     generation_retries_.erase(found);
             }});
    if (accepted == timing::CommandAcceptance::kAccepted) return Status::Ok();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_retries_.erase(rule_id);
    }
    return Status::Error(ErrorCode::kUnavailable, "周期生成重试注册命令未被接收");
}

bool ScheduleReminderService::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}
}  // namespace voicelife::schedule
