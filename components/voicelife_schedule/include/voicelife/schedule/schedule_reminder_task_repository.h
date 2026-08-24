#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/** @brief 提醒业务链的状态；该状态与底层 Timing task 生命周期分离。 */
enum class ScheduleReminderBusinessStatus {
    kScheduled = 1,
    kWaitingAcknowledgement = 2,
    kAcknowledged = 3,
    kExhausted = 4,
    kCancelled = 5,
    kSnoozed = 6,
};

/** @brief 已提交到本地提醒事实的用户动作类型。 */
enum class ScheduleReminderActionKind {
    kAcknowledge = 1,
    kSnooze = 2,
};

/** @brief 一次实际注册的底层 Timing task 状态。 */
enum class ScheduleReminderTimerStatus {
    kPending = 1,
    kTriggered = 2,
    kCancelled = 3,
    kCompleted = 4,
    kFailed = 5,
};

/** @brief 一条提醒链中的一次实际定时任务记录。 */
struct ScheduleReminderTask {
    int64_t id = 0;
    ScheduleId schedule_id = 0;
    std::string event;
    int64_t chain_id = 0;
    int attempt = 1;
    std::optional<std::string> timing_task_id;
    DateTime trigger_at;
    ScheduleReminderBusinessStatus business_status = ScheduleReminderBusinessStatus::kScheduled;
    ScheduleReminderTimerStatus timer_status = ScheduleReminderTimerStatus::kPending;
    std::optional<DateTime> triggered_at;
    std::optional<std::string> action_operation_id;
    std::optional<ScheduleReminderActionKind> action_kind;
    std::optional<DateTime> action_occurred_at;
    std::optional<DateTime> action_next_trigger_at;
    DateTime created_at;
    DateTime updated_at;
};

/** @brief 提醒任务的独立持久化接口；Schedule 本身不保存提醒运行态。 */
class ScheduleReminderTaskRepository {
   public:
    /** @brief 析构提醒任务仓储。 */
    virtual ~ScheduleReminderTaskRepository() = default;

    /** @brief 插入提醒任务。
     * @param task 待插入任务。
     * @return 插入后的任务或错误状态。
     */
    virtual Result<ScheduleReminderTask> Insert(const ScheduleReminderTask& task) = 0;
    /** @brief 更新提醒任务。
     * @param task 待更新任务。
     * @return 更新操作状态。
     */
    virtual Status Update(const ScheduleReminderTask& task) = 0;
    /** @brief 按标识查询任务。
     * @param id 任务标识。
     * @return 查询结果。
     */
    [[nodiscard]] virtual Result<ScheduleReminderTask> FindById(int64_t id) const = 0;
    /** @brief 按 Timing task 标识查询任务。
     * @param timing_task_id Timing 层不透明任务标识。
     * @return 查询结果。
     */
    [[nodiscard]] virtual Result<ScheduleReminderTask> FindByTimingTaskId(std::string_view timing_task_id) const = 0;
    /** @brief 按日程查询提醒任务。
     * @param schedule_id 日程标识。
     * @return 查询结果。
     */
    [[nodiscard]] virtual Result<std::vector<ScheduleReminderTask>> FindBySchedule(ScheduleId schedule_id) const = 0;
    /** @brief 查询全部提醒任务。
     * @return 查询结果。
     */
    [[nodiscard]] virtual Result<std::vector<ScheduleReminderTask>> FindAll() const = 0;
    /** @brief 查询时间范围内已触发任务。
     * @param from 起始时间（含）。
     * @param to 结束时间（含）。
     * @return 查询结果。
     */
    [[nodiscard]] virtual Result<std::vector<ScheduleReminderTask>> FindTriggered(DateTime from, DateTime to) const = 0;
};

}  // namespace voicelife::schedule
