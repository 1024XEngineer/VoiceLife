#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "voicelife/contracts/status.h"

namespace voicelife::board_esp {

/** @brief BMI270 加速度采样，单位为 m/s^2。 */
struct ImuAcceleration {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/**
 * @brief 基于加速度模长的非阻塞摇晃检测器。
 *
 * 先用慢速基线消除设备静止姿态，再要求连续样本超过阈值并施加冷却时间，
 * 防止设备放下或单次噪声产生重复事件。该类不依赖 ESP-IDF，便于主机契约测试。
 */
class ShakeDetector final {
   public:
    /**
     * @brief 创建基于加速度模长的摇晃检测器。
     * @param threshold_mps2 偏离慢速基线的触发阈值，单位为 m/s^2。
     * @param cooldown_ms 两次摇晃事件之间的最短间隔，单位为毫秒。
     */
    explicit ShakeDetector(float threshold_mps2 = 4.5F, uint32_t cooldown_ms = 1500);

    /**
     * @brief 推入一个采样，返回本采样是否触发一次摇晃事件。
     * @param acceleration 当前三轴加速度，单位为 m/s^2。
     * @param timestamp_ms 当前采样的单调时间戳，单位为毫秒。
     * @return 本采样触发新摇晃事件时返回 true。
     */
    [[nodiscard]] bool Push(ImuAcceleration acceleration, uint64_t timestamp_ms);

    /**
     * @brief 清除基线、连续计数和冷却状态。
     */
    void Reset();

   private:
    float threshold_mps2_;
    uint32_t cooldown_ms_;
    float baseline_magnitude_mps2_ = 0.0F;
    uint8_t active_samples_ = 0;
    uint64_t last_event_ms_ = 0;
    bool baseline_ready_ = false;
    bool event_seen_ = false;
};

/**
 * @brief SparkBot BMI270 采样适配器。
 *
 * 适配器复用音频初始化创建的 I2C0 master bus，不新建或删除共享总线；传感器
 * 采样在独立 200 Hz 任务中完成，事件回调只投递一个板级语义事件。
 */
class SparkBotImu final {
   public:
    /** @brief 摇晃事件回调类型。 */
    using ShakeCallback = std::function<void()>;

    /**
     * @brief 创建 SparkBot BMI270 采样适配器。
     * @param i2c_port 共享 I2C master bus 的逻辑端口号。
     * @param i2c_address 首选 BMI270 七位地址，默认值为 0x68。
     */
    SparkBotImu(uint8_t i2c_port = 0, uint8_t i2c_address = 0x68);
    /** @brief 停止采样并释放适配器资源。 */
    ~SparkBotImu();

    /** @brief 禁止复制构造，避免重复拥有传感器任务。 */
    SparkBotImu(const SparkBotImu&) = delete;
    /** @brief 禁止复制赋值，避免重复拥有传感器任务。 */
    SparkBotImu& operator=(const SparkBotImu&) = delete;

    /**
     * @brief 在共享 I2C 总线已就绪后启动 BMI270 采样。
     * @param on_shake 检测到摇晃时调用的板级语义回调。
     * @return 初始化并启动成功返回 OK，否则返回不可用或内部错误。
     */
    [[nodiscard]] Status Start(ShakeCallback on_shake);

    /** @brief 停止采样任务并释放 BMI270 设备句柄。 */
    void Stop();

    /**
     * @brief 当前是否已完成传感器初始化并运行采样任务。
     * @return 采样任务正在运行时返回 true。
     */
    [[nodiscard]] bool running() const;

   private:
    /** @brief BMI270 具体实现的私有前置声明。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::board_esp
