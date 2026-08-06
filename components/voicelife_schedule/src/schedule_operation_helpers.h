#pragma once

#include <string>

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/**
 * @brief 校验记录日程操作命令。
 * @param command 待校验的操作记录命令。
 * @return 参数合法时返回成功，否则返回参数错误状态。
 */
Status ValidateRecordScheduleOperationCommand(const RecordScheduleOperationCommand& command);

/**
 * @brief 创建记录日程操作的参数错误结果。
 * @param error 错误说明。
 * @return 不包含操作记录的失败结果。
 */
RecordScheduleOperationResult InvalidRecordScheduleOperationResult(std::string error);

}  // namespace voicelife::schedule
