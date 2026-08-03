#include <string>

#include "support/test_support.h"
#include "voicelife/schedule/schedule.h"

using voicelife::ErrorCode;
using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::SchedulePolicy;
using voicelife::test::Check;

int main() {
    SchedulePolicy policy;
    const CreateScheduleCommand valid{
        .request_id = "request-1",
        .title = "架构评审",
        .starts_at = 1785747600,
        .ends_at = 1785751200,
        .time_zone = "Asia/Shanghai",
    };

    const auto created = policy.Create(valid, "schedule-1", 1785740000);
    Check(created.ok(), "合法日程应创建成功");
    Check(created.value->id == "schedule-1", "日程应保留生成的标识");
    Check(created.value->created_at == 1785740000, "日程应使用注入的当前时间");

    auto invalid = valid;
    invalid.request_id.clear();
    Check(policy.Create(invalid, "schedule-2", 1).status.code == ErrorCode::kInvalidArgument, "request_id 不能为空");

    invalid = valid;
    invalid.title = std::string(101, 'x');
    Check(policy.Create(invalid, "schedule-2", 1).status.code == ErrorCode::kInvalidArgument, "标题不能超过 100 字节");

    invalid = valid;
    invalid.starts_at = 0;
    Check(policy.Create(invalid, "schedule-2", 1).status.code == ErrorCode::kInvalidArgument, "开始时间必须有效");

    invalid = valid;
    invalid.ends_at = invalid.starts_at - 1;
    Check(policy.Create(invalid, "schedule-2", 1).status.code == ErrorCode::kInvalidArgument,
          "结束时间不能早于开始时间");

    Check(policy.Create(valid, "", 1).status.code == ErrorCode::kInternal, "缺少生成标识应作为内部错误");
    return 0;
}
