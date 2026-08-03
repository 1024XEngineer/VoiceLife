#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_runner.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::test::Check;

namespace {
class Clock final : public voicelife::timing::TimingClockPort { public: int64_t Now() const override { return now; } int64_t now = 1000; };
class Ids final : public voicelife::timing::TimingIdGeneratorPort { public: std::string Next(const char* p) override { return std::string(p) + "-" + std::to_string(next_++); } private: int next_ = 1; };
class Events final : public voicelife::timing::TimingEventPort { public: voicelife::Status Publish(const voicelife::timing::TimingEvent&) override { return voicelife::Status::Ok(); } };
}  // namespace

int main() {
    using namespace voicelife::timing;
    voicelife::test::InMemoryTimingTaskStore store;
    Clock clock;
    Ids ids;
    Events events;
    DefaultTimingTaskService service(store, clock, ids, events);
    const int64_t start = 1785686400;
    const auto task = service.RegisterTimerTask({
        .schedule_id = "schedule-future", .start_at = start,
        .recurrence = {.frequency = RecurrenceFrequency::kDay, .start_at = start,
                       .time_zone = "Asia/Shanghai"},
    });
    Check(task.ok(), "周期任务应注册成功");

    const auto recurrence_only = service.UpdateTimerTask({
        .task_id = task.value->task_id, .scope = ChangeScope::kFuture,
        .effective_from = start + 86400, .recurrence = RecurrenceRule{
            .frequency = RecurrenceFrequency::kWeek, .start_at = start + 86400,
            .time_zone = "Asia/Shanghai", .by_weekdays = {2}},
    });
    Check(recurrence_only.ok(), "future 更新应允许只提供 recurrence");
    auto stored = store.FindTask(task.value->task_id);
    Check(stored.value->recurrence.frequency == RecurrenceFrequency::kDay &&
              stored.value->pending_recurrence.has_value() &&
              stored.value->pending_recurrence->frequency == RecurrenceFrequency::kWeek &&
              stored.value->pending_effective_from == start + 86400,
          "future 更新应保留当前规则并持久化边界后的待生效规则");
    const auto before_boundary = service.ListCalendarView({
        .range_start = start, .range_end = start + 86400,
    });
    Check(before_boundary.ok() && before_boundary.value->total == 1 &&
              before_boundary.value->occurrences.front().planned_start_at == start,
          "future 更新不得改变边界以前的 occurrence");

    TimingTaskRunner runner(store, clock, ids, events);
    clock.now = start - 600;
    Check(runner.PollDue().ok(), "边界前旧规则 occurrence 应正常物化");
    stored = store.FindTask(task.value->task_id);
    Check(stored.value->recurrence.frequency == RecurrenceFrequency::kWeek &&
              !stored.value->pending_recurrence.has_value(),
          "Runner 推进到 effective_from 时应切换为新 recurrence");

    const int64_t cancel_from = start + 15 * 86400;
    const auto cancelled = service.CancelTimerTask({
        .task_id = task.value->task_id, .scope = ChangeScope::kFuture,
        .effective_from = cancel_from,
    });
    Check(cancelled.ok() && cancelled.value->status == TimingTaskStatus::kActive,
          "future 取消不应终止边界以前的任务");
    const auto view = service.ListCalendarView({.range_start = start, .range_end = start + 30 * 86400});
    Check(view.ok() && !view.value->occurrences.empty(), "边界以前的 occurrence 应保留");
    for (const auto& occurrence : view.value->occurrences) {
        Check(occurrence.planned_start_at < cancel_from, "future 取消后不得返回边界以后的 occurrence");
    }
    return 0;
}
