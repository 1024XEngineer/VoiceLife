#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/// 提供日程创建、删除、修改、查询及操作记录业务。
class ScheduleService {
   public:
    /**
     * @brief 使用指定日程仓储构造服务。
     * @param repository 日程持久化仓储；其生命周期必须长于本服务。
     * @param operation_repository 日程操作持久化仓储；其生命周期必须长于本服务。
     */
    ScheduleService(ScheduleRepository& repository, ScheduleOperationRepository& operation_repository);

    /**
     * @brief 创建一条日程。
     * @param command 新日程的数据。
     * @return 创建结果，包含可能存在的冲突。
     */
    CreateScheduleResult create_schedule(const CreateScheduleCommand& command) const;

    /**
     * @brief 取消日程，但不自动删除关联提醒。
     * @param command 要取消的日程。
     * @return 删除结果。
     */
    DeleteScheduleResult delete_schedule(const DeleteScheduleCommand& command);

    /**
     * @brief 只更新日程中本次提供的字段。
     * @param command 要应用的日程变更。
     * @return 更新结果，包含可能存在的冲突。
     */
    UpdateScheduleResult update_schedule(const UpdateScheduleCommand& command);

    /**
     * @brief 使用筛选条件和分页参数查询日程。
     * @param command 查询筛选条件和分页边界。
     * @return 匹配的日程及总数。
     */
    QueryScheduleResult query_schedule(const QueryScheduleCommand& command) const;

    /**
     * @brief 记录一次创建、修改、删除或撤销操作。
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
    /// 日程增删改查使用的持久化仓储。
    ScheduleRepository& repository_;
    /// 日程操作记录和原子撤销使用的持久化仓储。
    ScheduleOperationRepository& operation_repository_;
};

}  // namespace voicelife::schedule
