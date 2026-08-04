#include "support/test_support.h"
#include "voicelife/timing/timing_task_service.h"

using voicelife::Status;
using voicelife::test::Check;
using voicelife::timing::DefaultTimingTaskService;
using voicelife::timing::RegisterTimerTaskCommand;
using voicelife::timing::TimingClockPort;
using voicelife::timing::TimingIdGeneratorPort;
using voicelife::timing::TimingTask;
using voicelife::timing::TimingTaskStatus;
using voicelife::timing::TimingTaskStorePort;

namespace {

class AcceptingTimingTaskStore final : public TimingTaskStorePort {
   public:
    Status SaveTask(const TimingTask&) override { return Status::Ok(); }
};

class FixedTimingClock final : public TimingClockPort {
   public:
    int64_t Now() const override { return 1785740000; }
};

class FixedTimingIdGenerator final : public TimingIdGeneratorPort {
   public:
    std::string NextTaskId() override { return "task-1"; }
};

}  // namespace

int main() {
    AcceptingTimingTaskStore store;
    FixedTimingClock clock;
    FixedTimingIdGenerator ids;
    DefaultTimingTaskService service(store, clock, ids);

    const auto registered = service.RegisterTimerTask({
        .schedule_id = "schedule-1",
        .start_at = 1785747600,
        .time_zone = "Asia/Shanghai",
    });

    Check(registered.ok(), "合法的一次性日程应注册成功");
    Check(!registered.value->task_id.empty(), "注册结果应返回任务标识");
    Check(registered.value->status == TimingTaskStatus::kActive, "新注册任务应处于 active 状态");
    Check(registered.value->next_trigger_at == 1785747600, "下一次触发时间应等于日程开始时间");
    return 0;
}
