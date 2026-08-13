#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/** @brief 提供日程操作记录、近期操作查询和撤销业务。 */
class ScheduleOperationService {
   public:
    /**
     * @brief 使用指定操作仓储构造服务。
     * @param operation_repository 日程操作持久化仓储；其生命周期必须长于本服务。
     */
    explicit ScheduleOperationService(ScheduleOperationRepository& operation_repository);

    /**
     * @brief 记录一次创建、修改、取消或撤销操作。
     * @param command 要持久化的操作详情。
     * @return 操作记录结果。
     */
    RecordScheduleOperationResult record_schedule_operation(const RecordScheduleOperationCommand& command);

    /**
     * @brief 查询当前时间往前十五分钟内的可撤销操作。
     * @return 按操作时间倒序排列的可撤销操作。
     */
    QueryRecentScheduleOperationResult query_recent_schedule_operation() const;

    /**
     * @brief 在十五分钟窗口内撤销指定操作。
     * @param command 要撤销的操作。
     * @return 撤销结果，成功时包含恢复的数据。
     */
    UndoScheduleOperationResult undo_schedule_operation(const UndoScheduleOperationCommand& command);

   private:
    ScheduleOperationRepository& operation_repository_;
};

}  // namespace voicelife::schedule
