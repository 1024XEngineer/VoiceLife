#include "voicelife/schedule/schedule_reminder_service.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace voicelife::schedule {
namespace {

using namespace std::chrono_literals;

DateTime SystemNow() {
    return std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now());
}

timing::TriggerAt ToTriggerAt(DateTime value) {
    return std::chrono::time_point_cast<std::chrono::microseconds>(value);
}

std::optional<int64_t> ParseTaskId(const timing::TaskId& task_id) {
    try {
        std::size_t consumed = 0;
        const int64_t value = std::stoll(task_id.Value(), &consumed);
        if (consumed != task_id.Value().size() || value <= 0) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::chrono::minutes RetryDelay(int failure_count) {
    if (failure_count <= 1) return 1min;
    if (failure_count == 2) return 5min;
    return 15min;
}

}  // namespace

ScheduleReminderService::ScheduleReminderService(
    ScheduleRepository& repository,
    ScheduleService& schedule_service,
    ScheduleRuleService& rule_service,
    timing::TimingTaskService& timing_service,
    ScheduleReminderSpeechPort& speech,
    NowProvider now_provider)
    : repository_(repository),
      schedule_service_(schedule_service),
      rule_service_(rule_service),
      timing_service_(timing_service),
      speech_(speech),
      now_provider_(now_provider ? std::move(now_provider) : NowProvider{SystemNow}) {}

Status ScheduleReminderService::Start() {
    const Result<std::vector<Schedule>> loaded = repository_.FindAll();
    if (!loaded.ok()) return loaded.status;

    int64_t maximum_persisted_task_id = 0;
    for (const Schedule& schedule : *loaded.value) {
        maximum_persisted_task_id =
            std::max(maximum_persisted_task_id, schedule.reminder_task_id.value_or(0));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return Status::Ok();
        running_ = true;
        last_task_id_ = std::max(
            maximum_persisted_task_id,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    Status first_failure = Status::Ok();
    for (const Schedule& schedule : *loaded.value) {
        if (schedule.status != ScheduleStatus::kActive) continue;
        const Status synchronized = SynchronizeSchedule(schedule.id);
        if (!synchronized.ok() && first_failure.ok()) first_failure = synchronized;
    }
    return first_failure;
}

void ScheduleReminderService::Stop() {
    std::vector<int64_t> task_ids;
    const Result<std::vector<Schedule>> loaded = repository_.FindAll();
    if (loaded.ok()) {
        for (const Schedule& schedule : *loaded.value) {
            if (schedule.reminder_task_id.has_value()) {
                task_ids.push_back(*schedule.reminder_task_id);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        for (const auto& [rule_id, retry] : generation_retries_) {
            (void)rule_id;
            task_ids.push_back(retry.task_id);
        }
        generation_retries_.clear();
    }

    for (int64_t value : task_ids) {
        const auto task_id = timing::TaskId::Create(std::to_string(value));
        if (!task_id.has_value()) continue;
        (void)timing_service_.CancelTask({
            .task_id = *task_id,
            .on_result = {},
        });
    }
}

Status ScheduleReminderService::SynchronizeSchedule(ScheduleId schedule_id) {
    if (!IsRunning()) {
        return Status::Error(ErrorCode::kUnavailable, "日程提醒服务尚未启动");
    }
    const Result<Schedule> loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;

    Schedule schedule = *loaded.value;
    const Status cancelled = CancelPersistedReminder(schedule);
    if (!cancelled.ok()) return cancelled;
    schedule.reminder_task_id = std::nullopt;

    if (schedule.status != ScheduleStatus::kActive ||
        !schedule.start_time.has_value() || *schedule.start_time <= Now()) {
        return Status::Ok();
    }
    return RegisterReminder(std::move(schedule));
}

Status ScheduleReminderService::CancelScheduleReminder(ScheduleId schedule_id) {
    const Result<Schedule> loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;
    return CancelPersistedReminder(*loaded.value);
}

Status ScheduleReminderService::SuspendRuleReminders(ScheduleRuleId rule_id) {
    if (rule_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零");
    }
    const Result<std::vector<Schedule>> loaded = repository_.FindAll();
    if (!loaded.ok()) return loaded.status;

    Status first_failure = Status::Ok();
    for (const Schedule& schedule : *loaded.value) {
        if (schedule.rule_id != rule_id || !schedule.reminder_task_id.has_value()) continue;
        const Status cancelled = CancelPersistedReminder(schedule);
        if (!cancelled.ok() && first_failure.ok()) first_failure = cancelled;
    }

    std::optional<int64_t> retry_task_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = generation_retries_.find(rule_id);
        if (found != generation_retries_.end()) {
            retry_task_id = found->second.task_id;
            generation_retries_.erase(found);
        }
    }
    if (retry_task_id.has_value()) {
        const auto task_id = timing::TaskId::Create(std::to_string(*retry_task_id));
        if (task_id.has_value() &&
            timing_service_.CancelTask({.task_id = *task_id, .on_result = {}}) ==
                timing::CommandAcceptance::kUnavailable &&
            first_failure.ok()) {
            first_failure = Status::Error(ErrorCode::kUnavailable, "周期生成重试取消命令未被接收");
        }
    }
    return first_failure;
}

Status ScheduleReminderService::SynchronizeRule(ScheduleRuleId rule_id) {
    if (rule_id <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "规则 ID 必须大于零");
    }
    const Result<std::vector<Schedule>> loaded = repository_.FindAll();
    if (!loaded.ok()) return loaded.status;

    Status first_failure = Status::Ok();
    for (const Schedule& schedule : *loaded.value) {
        if (schedule.rule_id != rule_id || schedule.status != ScheduleStatus::kActive) continue;
        const Status synchronized = SynchronizeSchedule(schedule.id);
        if (!synchronized.ok() && first_failure.ok()) first_failure = synchronized;
    }
    return first_failure;
}

DateTime ScheduleReminderService::Now() const { return now_provider_(); }

int64_t ScheduleReminderService::AllocateTaskId() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_task_id_ == std::numeric_limits<int64_t>::max()) {
        last_task_id_ = 1;
    } else {
        ++last_task_id_;
    }
    return last_task_id_;
}

Status ScheduleReminderService::ClearReminderTaskIfCurrent(
    ScheduleId schedule_id,
    int64_t task_id) {
    const Result<Schedule> loaded = repository_.FindById(schedule_id);
    if (!loaded.ok()) return loaded.status;
    if (loaded.value->reminder_task_id != task_id) return Status::Ok();
    Schedule updated = *loaded.value;
    updated.reminder_task_id = std::nullopt;
    updated.updated_at = Now();
    return repository_.Update(updated);
}

Status ScheduleReminderService::CancelPersistedReminder(Schedule schedule) {
    if (!schedule.reminder_task_id.has_value()) return Status::Ok();
    const int64_t task_value = *schedule.reminder_task_id;
    const auto task_id = timing::TaskId::Create(std::to_string(task_value));
    if (!task_id.has_value()) {
        return Status::Error(ErrorCode::kInternal, "持久化的提醒任务标识无效");
    }

    const timing::CommandAcceptance accepted = timing_service_.CancelTask({
        .task_id = *task_id,
        .on_result = {},
    });
    if (accepted == timing::CommandAcceptance::kUnavailable) {
        return Status::Error(ErrorCode::kUnavailable, "提醒任务取消命令未被接收");
    }
    return ClearReminderTaskIfCurrent(schedule.id, task_value);
}

Status ScheduleReminderService::RegisterReminder(Schedule schedule) {
    if (!schedule.start_time.has_value() || *schedule.start_time <= Now()) {
        return Status::Ok();
    }

    const int64_t task_value = AllocateTaskId();
    const auto task_id = timing::TaskId::Create(std::to_string(task_value));
    if (!task_id.has_value()) {
        return Status::Error(ErrorCode::kInternal, "无法创建提醒任务标识");
    }

    schedule.reminder_task_id = task_value;
    schedule.updated_at = Now();
    const Status persisted = repository_.Update(schedule);
    if (!persisted.ok()) return persisted;

    const timing::CommandAcceptance accepted = timing_service_.RegisterTask({
        .task_id = *task_id,
        .trigger_at = ToTriggerAt(*schedule.start_time),
        .callback = [this, schedule_id = schedule.id](
                        const timing::TaskId& fired_id,
                        timing::TriggerAt trigger_at) {
            (void)trigger_at;
            const std::optional<int64_t> parsed = ParseTaskId(fired_id);
            if (parsed.has_value()) HandleReminder(schedule_id, *parsed);
        },
        .on_result = [this, schedule_id = schedule.id, task_value](
                         timing::RegisterTaskResult result) {
            if (result == timing::RegisterTaskResult::kDuplicate) {
                (void)ClearReminderTaskIfCurrent(schedule_id, task_value);
            }
        },
    });
    if (accepted == timing::CommandAcceptance::kUnavailable) {
        (void)ClearReminderTaskIfCurrent(schedule.id, task_value);
        return Status::Error(ErrorCode::kUnavailable, "提醒任务注册命令未被接收");
    }
    return Status::Ok();
}

void ScheduleReminderService::HandleReminder(
    ScheduleId schedule_id,
    int64_t task_id) {
    if (!IsRunning()) return;
    const Result<Schedule> loaded = repository_.FindById(schedule_id);
    if (!loaded.ok() || loaded.value->status != ScheduleStatus::kActive ||
        loaded.value->reminder_task_id != task_id) {
        return;
    }

    const Schedule schedule = *loaded.value;
    const std::string reminder = "提醒：现在是「" + schedule.event + "」时间了";
    const Status spoken = speech_.SpeakScheduleReminder(reminder);
    if (spoken.ok()) {
        (void)schedule_service_.complete_schedule(schedule.id, task_id);
    } else {
        (void)ClearReminderTaskIfCurrent(schedule.id, task_id);
    }

    if (schedule.rule_id.has_value()) {
        GenerateNextInstance(*schedule.rule_id, 0);
    }
}

void ScheduleReminderService::GenerateNextInstance(
    ScheduleRuleId rule_id,
    int prior_failure_count) {
    if (!IsRunning()) return;
    const GenerateNextScheduleInstanceResult generated =
        rule_service_.generate_next_schedule_instance({.rule_id = rule_id});
    if (!generated.status.ok()) {
        (void)ScheduleGenerationRetry(rule_id, prior_failure_count + 1);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_retries_.erase(rule_id);
    }
    if (generated.schedule.has_value()) {
        (void)SynchronizeSchedule(generated.schedule->id);
    }
}

Status ScheduleReminderService::ScheduleGenerationRetry(
    ScheduleRuleId rule_id,
    int failure_count) {
    if (!IsRunning()) {
        return Status::Error(ErrorCode::kUnavailable, "日程提醒服务尚未启动");
    }
    const int64_t task_value = AllocateTaskId();
    const auto task_id = timing::TaskId::Create(std::to_string(task_value));
    if (!task_id.has_value()) {
        return Status::Error(ErrorCode::kInternal, "无法创建周期生成重试任务标识");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_retries_[rule_id] = {
            .task_id = task_value,
            .failure_count = failure_count,
        };
    }

    const timing::CommandAcceptance accepted = timing_service_.RegisterTask({
        .task_id = *task_id,
        .trigger_at = ToTriggerAt(Now() + RetryDelay(failure_count)),
        .callback = [this, rule_id, task_value](
                        const timing::TaskId& fired_id,
                        timing::TriggerAt trigger_at) {
            (void)trigger_at;
            const std::optional<int64_t> parsed = ParseTaskId(fired_id);
            if (!parsed.has_value() || *parsed != task_value) return;
            int failure_count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = generation_retries_.find(rule_id);
                if (found == generation_retries_.end() ||
                    found->second.task_id != task_value) {
                    return;
                }
                failure_count = found->second.failure_count;
                generation_retries_.erase(found);
            }
            GenerateNextInstance(rule_id, failure_count);
        },
        .on_result = [this, rule_id, task_value](
                         timing::RegisterTaskResult result) {
            if (result != timing::RegisterTaskResult::kDuplicate) return;
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = generation_retries_.find(rule_id);
            if (found != generation_retries_.end() &&
                found->second.task_id == task_value) {
                generation_retries_.erase(found);
            }
        },
    });
    if (accepted == timing::CommandAcceptance::kUnavailable) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = generation_retries_.find(rule_id);
        if (found != generation_retries_.end() &&
            found->second.task_id == task_value) {
            generation_retries_.erase(found);
        }
        return Status::Error(ErrorCode::kUnavailable, "周期生成重试注册命令未被接收");
    }
    return Status::Ok();
}

bool ScheduleReminderService::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

}  // namespace voicelife::schedule
