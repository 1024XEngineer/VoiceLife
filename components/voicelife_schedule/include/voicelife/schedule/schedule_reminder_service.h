#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_repository.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"

namespace voicelife::timing {
/** @brief 前向声明定时任务服务，避免日程公开接口依赖定时组件头文件。 */
class TimingTaskService;
}  // namespace voicelife::timing

namespace voicelife::schedule {

/** @brief 提交日程提醒文本并返回实际 TTS 请求结果。 */
class ScheduleReminderSpeechPort {
   public:
    /** @brief 析构提醒语音端口。 */
    virtual ~ScheduleReminderSpeechPort() = default;

    /** @brief 请求播报提醒文本。 @param text 完整提醒文本。 @return TTS 提交结果。 */
    virtual Status SpeakScheduleReminder(std::string_view text) = 0;
};

/** @brief 一次提醒触发事件的快照，供通知端口同步消费。 */
struct ReminderFireNotice {
    /** @brief 触发提醒的日程 ID。 */
    ScheduleId schedule_id;
    /** @brief 触发的提醒任务标识（首轮或推迟重复提醒）。 */
    int64_t task_id;
    /** @brief 日程标题。 */
    std::string event;
    /** @brief 本轮提醒的计划触发时间。 */
    DateTime trigger_time;
};

/**
 * @brief 接收日程提醒触发事件的端口。
 *
 * 提醒回调运行在定时任务执行上下文，实现必须快速返回：只做非阻塞入队，
 * 不得执行 TTS、数据库写入或任何可能阻塞的操作。
 */
class ScheduleReminderNotificationPort {
   public:
    /** @brief 析构提醒通知端口。 */
    virtual ~ScheduleReminderNotificationPort() = default;

    /** @brief 通知一次提醒已触发。 @param notice 提醒触发快照。 */
    virtual void NotifyReminderFired(const ReminderFireNotice& notice) = 0;
};

/** @brief 协调日程持久化、一次性定时任务、TTS 与周期实例生成。 */
class ScheduleReminderService final {
   public:
    using NowProvider = std::function<DateTime()>;

    /** @brief 构造日程提醒服务。
     * @param repository 日程仓储。
     * @param schedule_service 日程业务服务。
     * @param rule_service 周期规则业务服务。
     * @param timing_service 定时任务服务。
     * @param speech 提醒语音端口。
     * @param now_provider 当前时间提供者。
     * @param notification 可选的提醒通知端口；为空时只做语音提醒。 */
    ScheduleReminderService(ScheduleRepository& repository, ScheduleService& schedule_service,
                            ScheduleRuleService& rule_service, timing::TimingTaskService& timing_service,
                            ScheduleReminderSpeechPort& speech, NowProvider now_provider = {},
                            ScheduleReminderNotificationPort* notification = nullptr);

    /** @brief 启动服务并恢复全部 active 且未来到期的实例提醒。 @return 首个同步失败的错误，否则返回 Ok。 */
    Status Start();

    /** @brief 停止接收回调并取消当前服务持有的提醒与重试任务。 */
    void Stop();

    /** @brief 按最新持久化数据同步指定日程的提醒。 @param schedule_id 日程 ID。 @return 同步失败时的错误，否则返回 Ok。
     */
    Status SynchronizeSchedule(ScheduleId schedule_id);

    /** @brief 取消指定日程当前持久化的提醒任务。 @param schedule_id 日程 ID。 @return 取消失败时的错误，否则返回 Ok。
     */
    Status CancelScheduleReminder(ScheduleId schedule_id);

    /** @brief 在规则修改或取消前撤销其全部实例提醒，防止旧实例被删除后丢失任务标识。 @param rule_id 周期规则 ID。
     * @return 首个撤销失败的错误，否则返回 Ok。 */
    Status SuspendRuleReminders(ScheduleRuleId rule_id);

    /** @brief 为规则当前已经物化的 active 实例同步提醒。 @param rule_id 周期规则 ID。 @return
     * 首个同步失败的错误，否则返回 Ok。 */
    Status SynchronizeRule(ScheduleRuleId rule_id);

    /** @brief 推迟日程的下一次重复提醒十分钟。
     * @param schedule_id 日程 ID。
     * @return 成功时返回新的重复提醒触发时间；未启动返回 kUnavailable，日程已取消或推迟次数已达上限返回 kConflict，
     * 注册失败时回滚持久化状态并返回 kUnavailable。 */
    Result<DateTime> SnoozeScheduleReminder(ScheduleId schedule_id);

    /** @brief 确认日程提醒并取消其未触发的推迟重复提醒。
     * @param schedule_id 日程 ID。
     * @return 未启动返回 kUnavailable，其余失败返回对应错误。 */
    Status AcknowledgeScheduleReminder(ScheduleId schedule_id);

   private:
    /** @brief 单轮提醒最多接受的推迟次数；第 kMaxSnoozeCount + 1 次推迟被拒绝。 */
    static constexpr int kMaxSnoozeCount = 3;

    /** @brief 每次推迟后的重复提醒间隔。 */
    static constexpr std::chrono::minutes kSnoozeDelay{10};
    /** @brief 周期实例生成重试状态。 */
    struct RetryState {
        int64_t task_id = 0;
        int failure_count = 0;
    };

    DateTime Now() const;
    int64_t AllocateTaskId();
    Status ClearReminderTaskIfCurrent(ScheduleId schedule_id, int64_t task_id);
    Status CancelPersistedReminder(Schedule schedule);
    Status RegisterReminder(Schedule schedule);
    void HandleReminder(ScheduleId schedule_id, int64_t task_id);
    void GenerateNextInstance(ScheduleRuleId rule_id, int prior_failure_count);
    Status ScheduleGenerationRetry(ScheduleRuleId rule_id, int failure_count);
    Status CancelRepeatTask(const Schedule& schedule);
    Status ClearRepeatIfCurrent(ScheduleId schedule_id, int64_t task_id);
    Status RestoreRepeatReminder(ScheduleId schedule_id);
    void HandleRepeatReminder(ScheduleId schedule_id, int64_t task_id);
    bool IsRunning() const;

    ScheduleRepository& repository_;
    ScheduleService& schedule_service_;
    ScheduleRuleService& rule_service_;
    timing::TimingTaskService* timing_service_;
    ScheduleReminderSpeechPort& speech_;
    NowProvider now_provider_;
    ScheduleReminderNotificationPort* notification_;

    mutable std::mutex mutex_;
    bool running_ = false;
    int64_t last_task_id_ = 0;
    std::unordered_map<ScheduleRuleId, RetryState> generation_retries_;
};

}  // namespace voicelife::schedule
