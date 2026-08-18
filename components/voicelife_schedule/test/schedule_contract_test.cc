#include "support/test_support.h"
#include "voicelife/schedule/schedule_service.h"

using voicelife::schedule::CreateScheduleCommand;
using voicelife::schedule::QueryScheduleCommand;
using voicelife::schedule::ScheduleOperationType;
using voicelife::schedule::ScheduleStatus;
using voicelife::schedule::ScheduleStatusFilter;
using voicelife::schedule::UpdateScheduleCommand;
using voicelife::test::Check;

int main() {
    CreateScheduleCommand create;
    create.event = "架构评审";
    Check(create.event == "架构评审" && !create.ignore_conflict, "创建日程命令默认不忽略冲突");

    const UpdateScheduleCommand update;
    Check(!update.location.has_value() && !update.ignore_conflict, "修改日程命令默认不修改可选字段且不忽略冲突");
    Check(ScheduleStatus::kCompleted != ScheduleStatus::kCancelled, "已完成状态应是独立的日程状态");

    const QueryScheduleCommand query;
    Check(query.limit == 10 && query.offset == 0, "查询日程命令应提供默认分页参数");
    Check(query.status == ScheduleStatusFilter::kActive, "查询日程命令应默认筛选有效状态");
    Check(static_cast<int>(ScheduleStatus::kCancelled) == 2 && static_cast<int>(ScheduleStatus::kCompleted) == 3,
          "新增完成状态不应改变已取消状态的持久化值");
    Check(static_cast<int>(ScheduleOperationType::kUndo) == 4, "撤销操作应使用稳定的第四类持久化值");
    return 0;
}
