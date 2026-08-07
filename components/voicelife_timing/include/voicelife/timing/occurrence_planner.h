#pragma once

#include <cstdint>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

/**
 * @brief 在 UTC 时间范围内展开任务的 occurrence 计划。
 * @param task 要展开的一次性或周期任务；周期任务必须使用当前 MVP 支持的 +08:00 时区。
 * @param range_start 查询范围起点，包含该时刻。
 * @param range_end 查询范围终点，不包含该时刻。
 * @return 按时间升序排列的 UTC occurrence 时间戳，或参数/时区错误。
 */
Result<std::vector<int64_t>> PlanOccurrences(const TimingTask& task, int64_t range_start, int64_t range_end);

}  // namespace voicelife::timing
