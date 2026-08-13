#pragma once

#include <memory>

#include "voicelife/contracts/status.h"
#include "voicelife/timing/timing_runtime.h"

namespace voicelife::timing_esp {

using timing::CancelTaskCommand;
using timing::CommandAcceptance;
using timing::RegisterTaskCommand;
using timing::TimingTaskService;

/**
 * @brief ESP-IDF 平台上的 one-shot esp_timer 与 FreeRTOS Runner 运行时。
 *
 * 外部 RegisterTask/CancelTask 通过 FreeRTOS Queue 交给普通 Runner task；
 * esp_timer callback 只发送 Task Notification，不执行 task callback。
 */
class EspTimingTaskRuntime final : public TimingTaskService {
   public:
    /**
     * @brief 创建并启动 ESP Timing 运行时。
     * @return timer、Queue 和 Runner task 全部创建成功时返回实例，否则返回类型化失败。
     */
    static Result<std::unique_ptr<EspTimingTaskRuntime>> Create();

    /**
     * @brief 停止 Runner 并释放 timer、Queue 和 Task Notification 资源。
     *
     * 必须从 Runner task 之外销毁；销毁会等待已开始的 callback 与已接受命令处理完成。
     */
    ~EspTimingTaskRuntime() override;

    /** @brief 禁止拷贝构造。 */
    EspTimingTaskRuntime(const EspTimingTaskRuntime&) = delete;
    /** @brief 禁止拷贝赋值。 */
    EspTimingTaskRuntime& operator=(const EspTimingTaskRuntime&) = delete;

    /**
     * @brief 将一个一次性 task 注册命令提交给 ESP Runner。
     * @param command 待 Runner 应用的注册命令。
     * @return 命令进入 Runner 队列时返回 kAccepted；队列忙、已满或停机时返回 kUnavailable。
     */
    CommandAcceptance RegisterTask(RegisterTaskCommand command) override;

    /**
     * @brief 将一个 task 取消命令提交给 ESP Runner。
     * @param command 待 Runner 应用的取消命令。
     * @return 命令进入 Runner 队列时返回 kAccepted；队列忙、已满或停机时返回 kUnavailable。
     */
    CommandAcceptance CancelTask(CancelTaskCommand command) override;

   private:
    /// 封装 ESP-IDF timer、Queue 和 Runner task 的平台实现。
    class Impl;

    explicit EspTimingTaskRuntime(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::timing_esp
