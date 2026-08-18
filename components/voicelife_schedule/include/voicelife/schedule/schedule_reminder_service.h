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
     * @param now_provider 当前时间提供者。 */
    ScheduleReminderService(ScheduleRepository& repository, ScheduleService& schedule_service,
                            ScheduleRuleService& rule_service, timing::TimingTaskService& timing_service,
                            ScheduleReminderSpeechPort& speech, NowProvider now_provider = {});

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

   private:
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
    bool IsRunning() const;

    ScheduleRepository& repository_;
    ScheduleService& schedule_service_;
    ScheduleRuleService& rule_service_;
    timing::TimingTaskService* timing_service_;
    ScheduleReminderSpeechPort& speech_;
    NowProvider now_provider_;

    mutable std::mutex mutex_;
    bool running_ = false;
    int64_t last_task_id_ = 0;
    std::unordered_map<ScheduleRuleId, RetryState> generation_retries_;
};

}  // namespace voicelife::schedule
