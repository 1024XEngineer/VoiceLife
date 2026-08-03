#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

// 日程模块用例接口，仅定义能力边界，不包含具体存储和业务实现。
class ScheduleService {
   public:
    virtual ~ScheduleService() = default;

    // 创建一条日程。
    virtual CreateScheduleResult create_schedule(const CreateScheduleCommand&) = 0;

    // 取消指定日程，但不自动删除关联提醒。
    virtual DeleteScheduleResult delete_schedule(const DeleteScheduleCommand&) = 0;

    // 修改指定日程中已提供的字段。
    virtual UpdateScheduleResult update_schedule(const UpdateScheduleCommand&) = 0;

    // 按筛选条件查询日程。
    virtual QueryScheduleResult query_schedule(const QueryScheduleCommand&) const = 0;

    // 记录一次创建、修改或删除操作。
    virtual RecordScheduleOperationResult record_schedule_operation(const RecordScheduleOperationCommand&) = 0;

    // 查询最近十条可撤销的日程操作。
    virtual QueryRecentScheduleOperationResult query_recent_schedule_operation() const = 0;

    // 撤销最近十五分钟内的指定日程操作。
    virtual UndoScheduleOperationResult undo_schedule_operation(const UndoScheduleOperationCommand&) = 0;
};

}  // namespace voicelife::schedule
