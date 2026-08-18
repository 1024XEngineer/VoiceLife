#pragma once

#include <string>

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/**
 * @brief 校验记录操作命令。
 * @param command 待校验的操作记录命令。
 * @return 参数合法时返回成功，否则返回参数错误状态。
 */
Status ValidateRecordOperationCommand(const RecordOperationCommand& command);

/**
 * @brief 创建记录操作的参数错误结果。
 * @param error 错误说明。
 * @return 不包含操作记录的失败结果。
 */
RecordOperationResult InvalidRecordOperationResult(std::string error);

/**
 * @brief 校验查询操作命令。
 * @param command 待校验的查询命令。
 * @return 参数合法时返回成功，否则返回参数错误状态。
 */
Status ValidateQueryOperationCommand(const QueryOperationCommand& command);

/**
 * @brief 创建查询操作的参数错误结果。
 * @param error 错误说明。
 * @return 不包含操作记录且总条数为零的失败结果。
 */
QueryOperationResult InvalidQueryOperationResult(std::string error);

}  // namespace voicelife::schedule
