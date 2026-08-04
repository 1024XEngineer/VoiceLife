#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task_store.h"

namespace voicelife::timing {

/// 提供注册一次性定时任务所需的数据。
struct RegisterTimerTaskCommand {
    ScheduleId schedule_id{};
    int64_t start_at = 0;
    std::string time_zone = "Asia/Shanghai";
};

/// 返回新任务的标识、状态和下一次触发时间。
struct RegisterTimerTaskResult {
    TimingTaskId task_id{};
    TimingTaskStatus status = TimingTaskStatus::kActive;
    int64_t next_trigger_at = 0;
};

/// 定义定时任务模块对调用方公开的用例边界。
class TimingTaskService {
   public:
    /** @brief 允许通过接口类型释放服务实现。 */
    virtual ~TimingTaskService() = default;
    /**
     * @brief 注册一条一次性定时任务。
     * @param command 要注册的日程定时信息。
     * @return 注册结果或校验、持久化错误。
     */
    virtual Result<RegisterTimerTaskResult> RegisterTimerTask(const RegisterTimerTaskCommand& command) = 0;
};

/// 使用领域策略和注入端口实现定时任务用例。
class DefaultTimingTaskService final : public TimingTaskService {
   public:
    /**
     * @brief 使用指定端口创建服务。
     * @param store 任务存储。
     * @param clock 当前时间来源。
     * @param ids 任务标识生成器。
     */
    DefaultTimingTaskService(TimingTaskStorePort& store, TimingClockPort& clock, TimingIdGeneratorPort& ids)
        : store_(store), clock_(clock), ids_(ids) {}

    /**
     * @brief 注册并持久化一条一次性定时任务。
     * @param command 要注册的日程定时信息。
     * @return 注册结果或校验、持久化错误。
     */
    Result<RegisterTimerTaskResult> RegisterTimerTask(const RegisterTimerTaskCommand& command) override;

   private:
    TimingTaskStorePort& store_;
    TimingClockPort& clock_;
    TimingIdGeneratorPort& ids_;
};

}  // namespace voicelife::timing
