#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/// 日程模块用例接口，仅定义能力边界，不包含具体存储和业务实现。
class ScheduleService {
   public:
    /** @brief 允许通过接口类型释放服务。 */
    virtual ~ScheduleService() = default;

    /**
     * @brief 创建一条日程。
     * @param command 新日程的数据。
     * @return 创建结果，包含可能存在的冲突。
     */
    virtual CreateScheduleResult create_schedule(const CreateScheduleCommand& command) = 0;

    /**
     * @brief 取消日程，但不自动删除关联提醒。
     * @param command 要取消的日程。
     * @return 删除结果。
     */
    virtual DeleteScheduleResult delete_schedule(const DeleteScheduleCommand& command) = 0;

    /**
     * @brief 只更新日程中本次提供的字段。
     * @param command 要应用的日程变更。
     * @return 更新结果，包含可能存在的冲突。
     */
    virtual UpdateScheduleResult update_schedule(const UpdateScheduleCommand& command) = 0;

    /**
     * @brief 使用筛选条件和分页参数查询日程。
     * @param command 查询筛选条件和分页边界。
     * @return 匹配的日程及总数。
     */
    virtual QueryScheduleResult query_schedule(const QueryScheduleCommand& command) const = 0;

    /**
     * @brief 记录一次创建、修改或删除操作。
     * @param command 要持久化的操作详情。
     * @return 操作记录结果。
     */
    virtual RecordScheduleOperationResult record_schedule_operation(const RecordScheduleOperationCommand& command) = 0;

    /**
     * @brief 查询最近十条可撤销的操作。
     * @return 最近的可撤销操作。
     */
    virtual QueryRecentScheduleOperationResult query_recent_schedule_operation() const = 0;

    /**
     * @brief 在十五分钟窗口内撤销指定操作。
     * @param command 要撤销的操作。
     * @return 撤销结果，成功时包含恢复的数据。
     */
    virtual UndoScheduleOperationResult undo_schedule_operation(const UndoScheduleOperationCommand& command) = 0;
};

}  // namespace voicelife::schedule
