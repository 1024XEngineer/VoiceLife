#pragma once

#include <chrono>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_task.h"

namespace voicelife::timing {

/// 为 Timing Runner 提供绝对墙上时钟的 Port。
class ClockPort {
   public:
    /** @brief 允许通过接口指针安全释放实现。 */
    virtual ~ClockPort() = default;

    /**
     * @brief 读取当前绝对墙上时钟。
     * @return 当前 trigger_at 时间域中的绝对时间。
     */
    virtual TriggerAt Now() = 0;
};

/// 单个一次性定时器的调度 Port。
class OneShotTimerPort {
   public:
    /// 一次性 timer 到期时的轻量回调。
    using ExpiryCallback = std::function<void()>;

    /** @brief 允许通过接口指针安全释放实现。 */
    virtual ~OneShotTimerPort() = default;

    /**
     * @brief 绑定一次性 timer 到期回调。
     * @param callback 仅用于轻量通知 Runner 的回调。
     *
     * 仅允许在首次设置 timer 前绑定；运行中解除必须调用 ClearExpiryCallbackAndWait。
     */
    virtual void SetExpiryCallback(ExpiryCallback callback) = 0;

    /**
     * @brief 解除到期回调并等待已经开始的回调执行完毕。
     *
     * 返回前必须保证后续不会再开始旧回调；TimingTaskRuntime 析构依赖此所有权屏障。
     */
    virtual void ClearExpiryCallbackAndWait() = 0;

    /**
     * @brief 从现在起按相对延时设置一次唤醒。
     * @param delay 非负微秒延时。
     * @return timer 设置结果；平台暂不可用与内部错误使用不同错误码。
     */
    virtual Status ArmAfter(std::chrono::microseconds delay) = 0;

    /**
     * @brief 取消当前一次性唤醒。
     * @return timer 停止结果；原本未设置视为成功。
     */
    virtual Status Disarm() = 0;
};

/// 轻量通知普通 Runner 执行上下文的 Port。
class RunnerWakePort {
   public:
    /** @brief 允许通过接口指针安全释放实现。 */
    virtual ~RunnerWakePort() = default;

    /**
     * @brief 通知 Runner 处理命令或 timer 到期事件。
     *
     * 实现必须在本 Port 借用期内保持 Runner 已绑定，并保证通知不可失败；
     * 销毁方应先解除 timer callback，再解绑 Runner。
     */
    virtual void Notify() = 0;
};

/// 编排异步命令、到期推进和单个一次性唤醒的 Timing 运行时。
class TimingTaskRuntime final : public TimingTaskService {
   public:
    /**
     * @brief 绑定纯 Runner、墙上时钟、一次性 timer 和 Runner 唤醒 Port。
     * @param runner 独占 task 注册表的 Runner。
     * @param clock 提供绝对墙上时钟。
     * @param timer 管理唯一的一次性唤醒。
     * @param wake 轻量通知 Runner 执行上下文。
     *
     * 四个依赖均为借用引用，必须比本对象活得更久；析构时会先解除 timer callback。
     */
    TimingTaskRuntime(InMemoryTimingTaskRunner& runner, ClockPort& clock, OneShotTimerPort& timer,
                      RunnerWakePort& wake);

    /** @brief 解除 timer 回调并等待在途回调结束；不会销毁借用的依赖。 */
    ~TimingTaskRuntime() override;

    /**
     * @brief 接收一个一次性 task 注册命令并通知 Runner。
     * @param command 待 Runner 应用的注册命令。
     * @return 命令已被接收时返回 kAccepted。
     */
    CommandAcceptance RegisterTask(RegisterTaskCommand command) override;

    /**
     * @brief 接收一个 task 取消命令并通知 Runner。
     * @param command 待 Runner 应用的取消命令。
     * @return 命令已被接收时返回 kAccepted。
     */
    CommandAcceptance CancelTask(CancelTaskCommand command) override;

    /**
     * @brief 在 Runner 执行上下文中消费命令、推进到期批次并重设一次性唤醒。
     * @return timer 成功设置或停止时成功，否则保留平台错误分类。
     */
    Status ProcessWake();

   private:
    InMemoryTimingTaskRunner& runner_;
    ClockPort& clock_;
    OneShotTimerPort& timer_;
    RunnerWakePort& wake_;
};

}  // namespace voicelife::timing
