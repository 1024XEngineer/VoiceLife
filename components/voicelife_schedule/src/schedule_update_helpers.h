#pragma once

#include <optional>
#include <string>

#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/**
 * @brief 创建修改日程的参数错误结果。
 * @param error 错误说明。
 * @return 不包含日程和冲突的失败结果。
 */
UpdateScheduleResult InvalidUpdateScheduleResult(std::string error);

/**
 * @brief 判断状态是否属于日程模块支持的状态。
 * @param status 要校验的日程状态。
 * @return 状态为进行中、已取消或已完成时返回 true。
 */
bool IsSupportedScheduleStatus(ScheduleStatus status);

/**
 * @brief 将可清空的修改值应用到目标字段。
 * @tparam T 字段实际保存的数据类型。
 * @param update 外层表示是否修改、内层表示新值的修改数据。
 * @param target 要更新的日程字段。
 * @return 无返回值。
 */
template <typename T>
void ApplyNullableUpdate(const NullableScheduleUpdate<T>& update, std::optional<T>& target) {
    if (update.has_value()) target = *update;
}

}  // namespace voicelife::schedule
