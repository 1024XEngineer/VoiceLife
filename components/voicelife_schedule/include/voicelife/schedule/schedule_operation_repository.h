#pragma once

#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 定义日程操作日志的持久化能力。
 */
class ScheduleOperationRepository {
   public:
    /** @brief 允许通过接口类型释放操作仓储对象。 */
    virtual ~ScheduleOperationRepository() = default;

    /**
     * @brief 插入一条操作记录，生成 id 和 operated_at。
     * @param operation 待保存的操作；仓储负责生成操作标识和操作时间。
     * @return 实际保存后的完整操作记录，失败时返回错误状态。
     */
    virtual Result<OperationRecord> InsertOperation(const OperationRecord& operation) = 0;

    /**
     * @brief 按筛选条件查询操作记录，按 operated_at DESC, id DESC 排序。
     * @param query 查询筛选和分页条件。
     * @return 匹配的操作记录，失败时返回错误状态。
     */
    [[nodiscard]] virtual Result<std::vector<OperationRecord>> FindOperations(
        const QueryOperationCommand& query) const = 0;

    /**
     * @brief 统计满足筛选条件的总条数（不受分页影响），配合查询结果 total。
     * @param query 查询筛选条件，分页字段不参与统计。
     * @return 满足条件的总条数，失败时返回错误状态。
     */
    [[nodiscard]] virtual Result<int64_t> CountOperations(const QueryOperationCommand& query) const = 0;
};

}  // namespace voicelife::schedule
