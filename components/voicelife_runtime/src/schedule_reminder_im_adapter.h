#pragma once

#include <functional>
#include <utility>

#include "voicelife/im/im_action_channel.h"
#include "voicelife/im/im_action_executor.h"
#include "voicelife/im/im_clock.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/schedule/schedule_reminder_service.h"

namespace voicelife::runtime {

/** @brief 为提醒服务构造并提交 IM Gateway 通知；不修改 IM 模块内部实现。 */
class ImScheduleReminderNotification final : public schedule::ScheduleReminderNotificationPort {
   public:
    using ActionWindowSink = std::function<void(im::ActionWindow)>;

    ImScheduleReminderNotification(im::ImRuntime& runtime, ActionWindowSink action_window_sink = {})
        : runtime_(runtime), action_window_sink_(std::move(action_window_sink)) {}

    Status SendScheduleReminder(const schedule::Schedule& schedule,
                                const schedule::ScheduleReminderTask& task) override;

   private:
    im::ImRuntime& runtime_;
    ActionWindowSink action_window_sink_;
};

/** @brief 使用现有提醒服务执行 Gateway 下发的确认/延迟动作。 */
class ImScheduleReminderActionExecutor final : public im::ImActionExecutor {
   public:
    explicit ImScheduleReminderActionExecutor(schedule::ScheduleReminderService& service) : service_(service) {}

    contracts::im::ReminderActionResult Execute(
        const contracts::im::ReminderActionCommand& command) override;

   private:
    schedule::ScheduleReminderService& service_;
};

/** @brief 为 IM 动作通道提供可信系统时间的 ISO-8601 UTC 表示。 */
class EspScheduleReminderClock final : public im::ImClock {
   public:
    std::string NowIso() override;
};

}  // namespace voicelife::runtime
