#pragma once

#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 将一条日程操作追加到进程内模拟存储。
 * @param operation 待保存的操作；操作 ID 和时间会被模拟存储自动生成。
 * @return 保存后的完整操作记录；真实存储适配器接入前使用该结果。
 */
Result<OperationRecord> AppendMockScheduleOperation(OperationRecord operation);

/**
 * @brief 按指定时间追加一条日程操作，供模块测试构造确定性的操作窗口。
 * @param operation 待保存的操作；传入的操作 ID 会被模拟存储覆盖。
 * @param operated_at 要写入记录的确定性操作时间。
 * @return 保存后的完整操作记录；清空模拟存储后 ID 会重新从 1 开始分配。
 */
Result<OperationRecord> AppendMockScheduleOperationForTesting(OperationRecord operation, DateTime operated_at);

/**
 * @brief 读取进程内模拟存储中仍可撤销的全部日程操作。
 * @return 按写入时间从旧到新排列的有效操作记录副本。
 */
std::vector<OperationRecord> LoadMockScheduleOperations();

/**
 * @brief 按 ID 读取指定时间点仍可撤销的日程操作。
 * @param operation_id 要查找的操作记录 ID。
 * @param now 用于判断十五分钟撤销窗口的当前时间。
 * @return 成功时返回操作记录；记录不存在、已失效或已过期时返回失败。
 */
Result<OperationRecord> FindUndoableMockScheduleOperation(OperationId operation_id, DateTime now);

/**
 * @brief 原子地失效目标操作并追加一条新的撤销操作记录。
 * @param operation_id 要失效的原操作记录 ID。
 * @param undo_operation 待追加的撤销操作；操作 ID 和时间会被模拟存储覆盖。
 * @param now 撤销发生时间，同时用于复查原操作的十五分钟有效期。
 * @return 成功时返回新生成的撤销操作；失败时不会失效目标或追加记录。
 */
Result<OperationRecord> InvalidateMockScheduleOperationAndAppendUndo(OperationId operation_id,
                                                                     OperationRecord undo_operation, DateTime now);

/**
 * @brief 清空进程内模拟操作存储并重置 ID，供模块测试隔离全局状态。
 * @return 无返回值。
 */
void ResetMockScheduleOperationsForTesting();

}  // namespace voicelife::schedule
