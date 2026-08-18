#pragma once

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
#include "voicelife/timing/timing_task.h"

namespace voicelife::schedule {

/** @brief 提交日程提醒文本并返回实际 TTS 请求结果。 */
class ScheduleReminderSpeechPort {
   public:
    virtual ~ScheduleReminderSpeechPort() = default;

    /** @brief 请求播报提醒文本。 @param text 完整提醒文本。 @return TTS 提交结果。 */
    virtual Status SpeakScheduleReminder(std::string_view text) = 0;
};

/** @brief 协调日程持久化、一次性定时任务、TTS 与周期实例生成。 */
class ScheduleReminderService final {
   public:
    using NowProvider = std::function<DateTime()>;

    ScheduleReminderService(
        ScheduleRepository& repository,
        ScheduleService& schedule_service,
        ScheduleRuleService& rule_service,
        timing::TimingTaskService& timing_service,
        ScheduleReminderSpeechPort& speech,
        NowProvider now_provider = {});

    /** @brief 启动服务并恢复全部 active 且未来到期的实例提醒。 */
    Status Start();

    /** @brief 停止接收回调并取消当前服务持有的提醒与重试任务。 */
    void Stop();

    /** @brief 按最新持久化数据同步指定日程的提醒。 */
    Status SynchronizeSchedule(ScheduleId schedule_id);

    /** @brief 取消指定日程当前持久化的提醒任务。 */
    Status CancelScheduleReminder(ScheduleId schedule_id);

    /** @brief 在规则修改或取消前撤销其全部实例提醒，防止旧实例被删除后丢失任务标识。 */
    Status SuspendRuleReminders(ScheduleRuleId rule_id);

    /** @brief 为规则当前已经物化的 active 实例同步提醒。 */
    Status SynchronizeRule(ScheduleRuleId rule_id);

   private:
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
    bool IsRunning() const;

    ScheduleRepository& repository_;
    ScheduleService& schedule_service_;
    ScheduleRuleService& rule_service_;
    timing::TimingTaskService& timing_service_;
    ScheduleReminderSpeechPort& speech_;
    NowProvider now_provider_;

    mutable std::mutex mutex_;
    bool running_ = false;
    int64_t last_task_id_ = 0;
    std::unordered_map<ScheduleRuleId, RetryState> generation_retries_;
};

}  // namespace voicelife::schedule
