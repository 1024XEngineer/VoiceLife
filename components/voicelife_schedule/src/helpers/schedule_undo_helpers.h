#pragma once

#include <optional>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/// 已应用的模拟日程撤销状态，用于提交操作记录失败时恢复原状态。
struct AppliedScheduleUndo {
    std::optional<Schedule> before;
    std::optional<Schedule> after;
};

/**
 * @brief 校验撤销日程操作命令。
 * @param command 待校验的撤销命令。
 * @return 参数合法时返回成功，否则返回参数错误状态。
 */
Status ValidateUndoScheduleOperationCommand(const UndoScheduleOperationCommand& command);

/**
 * @brief 根据目标操作应用一次模拟日程撤销。
 * @param operation 要撤销的操作记录。
 * @return 成功时返回撤销前后的日程状态，失败时返回日程逆操作错误。
 */
Result<AppliedScheduleUndo> ApplyMockScheduleUndo(const OperationRecord& operation);

/**
 * @brief 在撤销操作记录提交失败时恢复模拟日程原状态。
 * @param applied 已应用的撤销前后状态。
 * @return 回滚成功时返回成功，否则返回模拟日程写入错误。
 */
Status RollbackMockScheduleUndo(const AppliedScheduleUndo& applied);

/**
 * @brief 创建不包含操作和日程数据的撤销失败结果。
 * @param status 撤销失败状态。
 * @return undone 为 false 并携带错误说明的结果。
 */
UndoScheduleOperationResult FailedUndoScheduleOperationResult(Status status);

}  // namespace voicelife::schedule
