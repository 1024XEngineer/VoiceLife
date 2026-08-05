#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::UpdateScheduleCommand;
using voicelife::test::Check;

int main() {
    CreateScheduleCommand create;
    create.event = "架构评审";
    Check(create.event == "架构评审" && !create.ignore_conflict, "创建日程命令默认不忽略冲突");

    const UpdateScheduleCommand update;
    Check(!update.location.has_value() && !update.status.has_value() && !update.ignore_conflict,
          "修改日程命令默认不修改可选字段且不忽略冲突");
    Check(ScheduleStatus::kComplete != ScheduleStatus::kCancelled, "已完成状态应是独立的日程状态");

    const QueryScheduleCommand query;
    Check(query.limit == 10 && query.offset == 0, "查询日程命令应提供默认分页参数");
    return 0;
}
