#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "voicelife/schedule/schedule_results.h"

namespace voicelife::schedule {

/**
 * @brief 去除文本两端的 ASCII 空白字符。
 * @param value 要清理的文本。
 * @return 清理后的文本。
 */
std::string TrimScheduleText(std::string_view value);

/**
 * @brief 计算 UTF-8 文本的字符数量。
 * @param value 要统计的 UTF-8 文本。
 * @return 不单独计算延续字节的字符数量。
 */
std::size_t ScheduleTextLength(std::string_view value);

/**
 * @brief 创建日程参数错误结果。
 * @param error 错误说明。
 * @return 不包含日程、冲突和临近日程的失败结果。
 */
CreateScheduleResult InvalidCreateScheduleResult(std::string error);

}  // namespace voicelife::schedule
