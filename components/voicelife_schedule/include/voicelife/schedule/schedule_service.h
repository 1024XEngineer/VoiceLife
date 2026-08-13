#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/// 提供一次性日程创建、取消、修改和查询业务。
class ScheduleService {
   public:
    /**
     * @brief 使用指定日程仓储构造服务。
     * @param repository 日程持久化仓储；其生命周期必须长于本服务。
     */
    explicit ScheduleService(ScheduleRepository& repository);

    /**
     * @brief 创建一条日程。
     * @param command 新日程的数据。
     * @return 创建结果，包含可能存在的冲突。
     */
    CreateScheduleResult create_schedule(const CreateScheduleCommand& command) const;

    /**
     * @brief 取消日程，但不自动删除关联提醒。
     * @param command 要取消的日程。
     * @return 取消结果。
     */
    CancelScheduleResult cancel_schedule(const CancelScheduleCommand& command);

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

   private:
    /// 一次性日程创建、取消、修改和查询使用的持久化仓储。
    ScheduleRepository& repository_;
};

}  // namespace voicelife::schedule
