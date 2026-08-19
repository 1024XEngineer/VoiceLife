#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/im/im_action_channel.h"
#include "voicelife/im/im_action_command_stream.h"
#include "voicelife/im/im_action_executor.h"
#include "voicelife/im/im_clock.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/schedule/schedule_reminder_service.h"

namespace voicelife::reminder_im {

/// 按提醒触发标识创建一次动作流连接的工厂。
using ReminderActionStreamFactory =
    std::function<std::unique_ptr<im::ImActionCommandStream>(const std::string& reminder_trigger_id)>;

/**
 * @brief 桥接日程提醒触发事件与 IM 强提醒动作窗口。
 *
 * 实现提醒通知端口（定时器回调上下文，必须非阻塞入队）与动作执行端口
 * （动作窗口内由 ImActionChannel 串行调用）。RunWorkerLoop 在独立任务中
 * 阻塞消费队列：先提交强提醒通知，受理结果携带 actionStream 时进入动作
 * 窗口，窗口结束或到期后处理下一条触发。
 */
class ImReminderBridge final : public schedule::ScheduleReminderNotificationPort, public im::ImActionExecutor {
   public:
    /**
     * @brief 创建桥。
     * @param runtime IM 运行时，提供 ready 状态、上报通道、userId 与 deviceId。
     * @param credentials 设备凭据，用于动作流连接与命令归属校验。
     * @param clock 当前时间来源。
     * @param reminder_service 日程提醒服务；可为空，随后用 SetScheduleReminderService 注入。
     * @param stream_factory 动作流工厂。
     * @param reconnect_delay 断线重连退避；测试可缩短。
     */
    ImReminderBridge(im::ImRuntime& runtime, im::ImCredentialProvider& credentials, im::ImClock& clock,
                     schedule::ScheduleReminderService* reminder_service, ReminderActionStreamFactory stream_factory,
                     std::chrono::milliseconds reconnect_delay = std::chrono::seconds(2));

    /** @brief 提醒触发回调：只做非阻塞入队，队列满时静默丢弃并计数。 */
    void NotifyReminderFired(const schedule::ReminderFireNotice& notice) override;

    /**
     * @brief 阻塞消费队列直到停止请求且队列清空。
     * @note 必须在独立任务中运行；调用方负责在停止请求后等待其返回。
     */
    void RunWorkerLoop();

    /** @brief 请求停止：中断阻塞读取与退避睡眠，队列清空后 worker 退出。 */
    void RequestStop();

    /** @brief 查询是否已请求停止。 */
    bool IsStopRequested() const;

    /** @brief 设置日程提醒服务；允许构造后注入以解开环形接线。 @param service 服务指针。 */
    void SetScheduleReminderService(schedule::ScheduleReminderService* service);

    /** @brief 返回因队列满而被丢弃的触发事件数。 */
    std::size_t dropped_fires() const { return dropped_fires_.load(std::memory_order_relaxed); }

    /** @brief 执行网关下发的提醒动作命令并返回回传结果。 */
    contracts::im::ReminderActionResult Execute(const contracts::im::ReminderActionCommand& command) override;

   private:
    /// 当前动作窗口：触发标识到日程标识的映射，仅由 worker 线程访问。
    struct ActiveWindow {
        std::string reminder_trigger_id;
        schedule::ScheduleId schedule_id;
    };
    /** @brief 处理一条触发事件：提交通知并在受理窗口内等待动作。 */
    void HandleFire(const schedule::ReminderFireNotice& notice);
    /** @brief 驱动一个动作窗口直到结束、到期或停止请求；断线按退避重连。 */
    void RunActionWindow(const im::ActionWindow& window);
    /** @brief 阻塞等待一条触发事件；停止请求且队列清空时返回 false。 */
    bool WaitForItem(schedule::ReminderFireNotice& out);

    /// 入队容量：溢出静默丢弃，防止定时器上下文长期阻塞。
    static constexpr std::size_t kQueueCapacity = 4;

    im::ImRuntime& runtime_;
    im::ImCredentialProvider& credentials_;
    im::ImClock& clock_;
    schedule::ScheduleReminderService* reminder_service_;
    ReminderActionStreamFactory stream_factory_;
    std::chrono::milliseconds reconnect_delay_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<schedule::ReminderFireNotice> queue_;
    bool stop_ = false;
    std::atomic<std::size_t> dropped_fires_{0};
    std::optional<ActiveWindow> active_window_;
};

}  // namespace voicelife::reminder_im
