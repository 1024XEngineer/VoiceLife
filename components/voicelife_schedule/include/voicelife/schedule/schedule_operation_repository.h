#pragma once

#include <optional>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/** @brief 描述一次撤销提交后的领域结果。 */
struct UndoOperationResult {
    /** @brief 被撤销的原操作记录。 */
    OperationRecord operation;
    /** @brief 撤销完成后的日程；撤销创建操作时为空。 */
    std::optional<Schedule> schedule;
};

/**
 * @brief 定义日程操作记录和原子撤销所需的持久化能力。
 *
 * 撤销由仓储在单个事务内完成日程逆操作、原记录失效和撤销记录写入，
 * 调用方不需要也不能拆分这些步骤。
 */
class ScheduleOperationRepository {
   public:
    /** @brief 允许通过接口类型释放操作仓储对象。 */
    virtual ~ScheduleOperationRepository() = default;

    /**
     * @brief 插入一条日程操作记录。
     * @param operation 待保存的操作；仓储负责生成操作标识和操作时间。
     * @return 实际保存后的完整操作记录，失败时返回错误状态。
     */
    virtual Result<OperationRecord> InsertOperation(const OperationRecord& operation) = 0;

    /**
     * @brief 查询指定时间点往前十五分钟闭区间内仍有效的操作。
     * @param now 查询窗口的结束时间。
     * @return 按操作时间和标识倒序排列的操作记录，失败时返回错误状态。
     */
    [[nodiscard]] virtual Result<std::vector<OperationRecord>> FindRecentOperations(DateTime now) const = 0;

    /**
     * @brief 原子撤销指定操作并写入一条新的撤销记录。
     * @param operation_id 要撤销的有效操作标识。
     * @param now 撤销发生时间，同时作为十五分钟有效期的结束边界。
     * @return 被撤销的原操作及撤销后的日程，失败时不保留部分修改。
     */
    virtual Result<UndoOperationResult> UndoOperation(OperationId operation_id, DateTime now) = 0;
};

}  // namespace voicelife::schedule
