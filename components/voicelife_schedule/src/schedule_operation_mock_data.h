#pragma once

#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 将一条日程操作追加到有界的进程内模拟存储。
 * @param operation 待保存的操作；操作 ID 和时间会被模拟存储自动生成。
 * @return 保存后的完整操作记录；真实存储适配器接入前使用该结果。
 */
Result<OperationRecord> AppendMockScheduleOperation(OperationRecord operation);

/**
 * @brief 读取进程内模拟存储中的最近日程操作。
 * @return 按写入时间从旧到新排列的操作记录副本。
 */
std::vector<OperationRecord> LoadMockScheduleOperations();

}  // namespace voicelife::schedule
