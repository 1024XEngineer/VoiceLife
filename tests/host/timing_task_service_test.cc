#include "support/test_support.h"
#include "support/timing_fakes.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::test::Check;

namespace {

class FixedClock final : public voicelife::timing::TimingClockPort {
   public:
    int64_t Now() const override { return 1000; }
};

class SequentialIds final : public voicelife::timing::TimingIdGeneratorPort {
   public:
    std::string Next(const char* prefix) override { return std::string(prefix) + "-" + std::to_string(next_++); }

   private:
    int next_ = 1;
};

class RecordingEvents final : public voicelife::timing::TimingEventPort {
   public:
    voicelife::Status Publish(const voicelife::timing::TimingEvent& event) override {
        events.push_back(event);
        return voicelife::Status::Ok();
    }
    std::vector<voicelife::timing::TimingEvent> events;
};

}  // namespace

int main() {
    using namespace voicelife::timing;
    voicelife::test::InMemoryTimingTaskStore store;
    FixedClock clock;
    SequentialIds ids;
    RecordingEvents events;
    DefaultTimingTaskService service(store, clock, ids, events);

    const auto registered = service.RegisterTimerTask({
        .schedule_id = "schedule-1",
        .start_at = 2000,
        .time_zone = "Asia/Shanghai",
    });
    Check(registered.ok(), "合法任务应注册成功");
    Check(registered.value->next_trigger_at == 2000, "首次触发时间应等于开始时间");

    const auto task = store.FindTask(registered.value->task_id);
    Check(task.ok() && task.value->status == TimingTaskStatus::kActive, "任务应持久化为 active");
    const auto rules = store.ListRules(registered.value->task_id);
    Check(rules.ok() && rules.value->size() == 2, "注册应原子创建两条默认提醒规则");
    Check((*rules.value)[0].type == ReminderType::kWeak && (*rules.value)[0].offset_minutes == -10,
          "默认弱提醒应提前十分钟");
    Check((*rules.value)[1].type == ReminderType::kStrong && (*rules.value)[1].offset_minutes == 0,
          "默认强提醒应在开始时触发");
    return 0;
}
