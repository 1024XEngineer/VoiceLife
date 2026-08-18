#pragma once

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_operation_repository.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/** @brief 提供日程操作记录和查询业务。 */
class ScheduleOperationService {
   public:
    /**
     * @brief 使用指定操作仓储构造服务。
     * @param operation_repository 日程操作持久化仓储；其生命周期必须长于本服务。
     */
    explicit ScheduleOperationService(ScheduleOperationRepository& operation_repository);

    /**
     * @brief 记录一次创建、修改或删除操作。
     * @param command 要持久化的操作详情。
     * @return 操作记录结果，成功时包含仓储盖章后的完整记录。
     */
    RecordOperationResult record_operation(const RecordOperationCommand& command);

    /**
     * @brief 按筛选条件查询操作记录。
     * @param command 查询筛选和分页条件。
     * @return 匹配的操作记录及不受分页影响的总体条数。
     */
    QueryOperationResult query_operations(const QueryOperationCommand& command) const;

   private:
    ScheduleOperationRepository& operation_repository_;
};

}  // namespace voicelife::schedule
