#include "support/test_support.h"
#include "voicelife/timing/timing_task.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::timing::RegisterTimingTaskCommand;
using voicelife::timing::TimingPolicy;

int main() {
    TimingPolicy policy;
    const RegisterTimingTaskCommand valid{
        .schedule_id = "schedule-1",
        .starts_at = 1785747600,
        .time_zone = "Asia/Shanghai",
    };

    const auto created = policy.Register(valid, "task-1", 1785740000);
    Check(created.ok(), "合法定时任务应注册成功");
    Check(created.value->schedule_id == "schedule-1", "定时任务应关联日程");
    Check(created.value->created_at == 1785740000, "定时任务应使用注入的当前时间");

    auto invalid = valid;
    invalid.schedule_id.clear();
    Check(policy.Register(invalid, "task-2", 1).status.code == ErrorCode::kInvalidArgument, "定时任务必须关联日程");

    invalid = valid;
    invalid.starts_at = 0;
    Check(policy.Register(invalid, "task-2", 1).status.code == ErrorCode::kInvalidArgument, "定时任务开始时间必须有效");

    Check(policy.Register(valid, "", 1).status.code == ErrorCode::kInvalidArgument, "定时任务必须有标识");
    return 0;
}
