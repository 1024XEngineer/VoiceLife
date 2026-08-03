#include <optional>
#include <string>
#include <utility>

#include "support/calendar_fakes.h"
#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Result;
using voicelife::Status;
using voicelife::application::CalendarApplication;
using voicelife::application::StoredCalendarEntry;
using voicelife::test::Check;
using voicelife::test::FixedClock;
using voicelife::test::RecordingCalendarStore;
using voicelife::test::RecordingNotifications;
using voicelife::test::SequenceIds;

namespace {

StoredCalendarEntry ExistingEntry() {
    return {
        .schedule =
            {
                .id = "schedule-existing",
                .request_id = "request-existing",
                .title = "既有日程",
                .starts_at = 1785747600,
                .ends_at = 0,
                .time_zone = "Asia/Shanghai",
                .status = voicelife::schedule::ScheduleStatus::kActive,
                .created_at = 1785740000,
            },
        .timing_task =
            {
                .id = "task-existing",
                .schedule_id = "schedule-existing",
                .next_trigger_at = 1785747600,
                .time_zone = "Asia/Shanghai",
                .status = voicelife::timing::TimingTaskStatus::kActive,
                .created_at = 1785740000,
            },
    };
}

voicelife::schedule::CreateScheduleCommand Command(std::string request_id, std::string title = "架构评审") {
    return {
        .request_id = std::move(request_id),
        .title = std::move(title),
        .starts_at = 1785747600,
        .ends_at = 0,
        .time_zone = "Asia/Shanghai",
    };
}

class ConcurrentReplayStore final : public voicelife::application::CalendarStorePort {
   public:
    explicit ConcurrentReplayStore(StoredCalendarEntry existing) : existing_(std::move(existing)) {}

    Result<std::optional<StoredCalendarEntry>> FindByRequestId(const std::string&) override {
        ++find_count_;
        return Result<std::optional<StoredCalendarEntry>>::Success(find_count_ == 1 ? std::nullopt
                                                                                    : std::optional(existing_));
    }

    Status SaveScheduleWithTimingTask(const StoredCalendarEntry&) override {
        return Status::Error(ErrorCode::kConflict, "并发请求已先完成");
    }

   private:
    int find_count_ = 0;
    StoredCalendarEntry existing_;
};

}  // namespace

int main() {
    FixedClock clock(1785740000);
    SequenceIds ids;
    RecordingCalendarStore store;
    RecordingNotifications notifications;
    CalendarApplication calendar(store, notifications, ids, clock);

    const auto created = calendar.CreateSchedule(Command("request-1"));
    Check(created.ok() && !created.value->duplicate, "首次请求应创建日程");
    Check(store.save_count == 1 && store.saved.has_value(), "日程与定时任务应通过一个原子 Port 保存");
    Check(store.saved->schedule.created_at == 1785740000, "Application 应使用独立 ClockPort");
    Check(notifications.count == 1 && created.value->notification_accepted, "保存成功后应发布通知意图");

    RecordingCalendarStore duplicate_store;
    duplicate_store.existing = ExistingEntry();
    RecordingNotifications duplicate_notifications;
    SequenceIds duplicate_ids;
    CalendarApplication duplicate_calendar(duplicate_store, duplicate_notifications, duplicate_ids, clock);
    const auto duplicate = duplicate_calendar.CreateSchedule(Command("request-existing", "既有日程"));
    Check(duplicate.ok() && duplicate.value->duplicate, "已有 request_id 应幂等返回");
    Check(duplicate_store.save_count == 0 && duplicate_notifications.count == 0, "幂等重放不能再次保存或发送通知");

    ConcurrentReplayStore replay_store(ExistingEntry());
    RecordingNotifications replay_notifications;
    SequenceIds replay_ids;
    CalendarApplication replay_calendar(replay_store, replay_notifications, replay_ids, clock);
    const auto replay = replay_calendar.CreateSchedule(Command("request-existing", "并发架构评审"));
    Check(replay.ok() && replay.value->duplicate, "并发保存冲突应回读为幂等结果");
    Check(replay.value->schedule_id == "schedule-existing", "并发回读应返回先完成的日程");
    Check(replay_notifications.count == 0, "并发重放不能重复通知");

    RecordingCalendarStore degraded_store;
    RecordingNotifications rejecting_notifications;
    rejecting_notifications.result = Status::Error(ErrorCode::kUnavailable, "通知通道不可用");
    SequenceIds degraded_ids;
    CalendarApplication degraded_calendar(degraded_store, rejecting_notifications, degraded_ids, clock);
    const auto degraded = degraded_calendar.CreateSchedule(Command("request-notification-down", "通知降级评审"));
    Check(degraded.ok() && !degraded.value->notification_accepted, "通知失败不能伪装成本地业务失败");
    Check(degraded_store.save_count == 1, "通知失败后本地事实应保留");

    RecordingCalendarStore failing_store;
    failing_store.find_result = Status::Error(ErrorCode::kUnavailable, "存储不可用");
    RecordingNotifications unused_notifications;
    SequenceIds unused_ids;
    CalendarApplication failing_calendar(failing_store, unused_notifications, unused_ids, clock);
    Check(failing_calendar.CreateSchedule(Command("request-failed")).status.code == ErrorCode::kUnavailable,
          "存储查询失败应原样返回");
    return 0;
}
