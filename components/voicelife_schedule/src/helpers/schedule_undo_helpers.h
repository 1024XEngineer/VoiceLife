#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/**
 * @brief 校验撤销日程操作命令。
 * @param command 待校验的撤销命令。
 * @return 参数合法时返回成功，否则返回参数错误状态。
 */
Status ValidateUndoScheduleOperationCommand(const UndoScheduleOperationCommand& command);

/**
 * @brief 创建不包含操作和日程数据的撤销失败结果。
 * @param status 撤销失败状态。
 * @return undone 为 false 并携带错误说明的结果。
 */
UndoScheduleOperationResult FailedUndoScheduleOperationResult(Status status);

}  // namespace voicelife::schedule
