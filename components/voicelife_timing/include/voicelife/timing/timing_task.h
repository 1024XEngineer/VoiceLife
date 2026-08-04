#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_types.h"

namespace voicelife::timing {

/// 提供注册定时任务所需的数据。
struct RegisterTimingTaskCommand {
    std::string schedule_id;
    int64_t starts_at = 0;
    std::string time_zone;
};

/// 执行定时领域校验并构造任务。
class TimingPolicy {
   public:
    /**
     * @brief 为日程注册第一条定时任务。
     * @param command 要注册的日程定时信息。
     * @param task_id 分配给新任务的 ID。
     * @param now 当前 Unix 秒级时间戳。
     * @return 注册成功的任务，或校验失败结果。
     */
    Result<TimingTask> Register(const RegisterTimingTaskCommand& command, std::string task_id, int64_t now) const;
};

}  // namespace voicelife::timing
