#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "voicelife/contracts/status.h"

namespace voicelife::board_esp {

/** @brief 三轴加速度，单位 m/s²。 */
struct SparkBotAcceleration {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/** @brief 三轴角速度，单位 °/s。 */
struct SparkBotGyroscope {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/** @brief 同一采样时刻的加速度和角速度。 */
struct SparkBotImuSample {
    SparkBotAcceleration acceleration;
    SparkBotGyroscope gyroscope;
};

/**
 * @brief 基于去重力线性加速度峰值的摇晃检测。
 *
 * 姿态翻转会改变重力方向，但不会产生交替的线性冲量。检测器使用低通
 * 重力估计、短窗口内的三次交替峰值和角速度辅助门槛，避免把翻转或单次
 * 碰撞误判为摇晃。设备任务和 host 回归测试共用同一套边界规则。
 */
class SparkBotMotionDetector final {
   public:
    /**
     * @brief 输入一个带单调时间戳的样本。
     * @param sample 同时刻采集的加速度和角速度。
     * @param timestamp_ms 单调递增的采样时间戳（毫秒）。
     * @return 本次是否触发摇晃。
     */
    bool Update(SparkBotImuSample sample, uint64_t timestamp_ms);

    /** @brief 清除基线、连续计数和冷却状态。 */
    void Reset();

   private:
    static constexpr float kLinearAccelerationThresholdMps2 = 3.4F;
    static constexpr float kLinearAccelerationRearmMps2 = 1.8F;
    static constexpr float kDynamicAccelerationThresholdMps2 = 1.3F;
    static constexpr float kDynamicAccelerationRearmMps2 = 0.7F;
    static constexpr float kAngularRateThresholdDps = 70.0F;
    static constexpr uint8_t kRequiredAlternatingPulses = 3;
    static constexpr uint64_t kMaxMotionWindowMs = 650;
    static constexpr uint64_t kMinPulseGapMs = 35;
    static constexpr uint64_t kMaxPulseGapMs = 260;
    static constexpr float kNominalGravityMps2 = 9.80665F;
    static constexpr float kBaselineToleranceMps2 = 2.5F;
    static constexpr uint64_t kCooldownMs = 2500;
    static constexpr float kGravityAlpha = 0.08F;

    bool has_gravity_ = false;
    SparkBotAcceleration gravity_{};
    float last_pulse_direction_ = 0.0F;
    uint8_t pulse_count_ = 0;
    bool pulse_armed_ = true;
    uint64_t motion_started_ms_ = 0;
    uint64_t last_pulse_ms_ = 0;
    bool has_timestamp_ = false;
    uint64_t last_timestamp_ms_ = 0;
    uint64_t cooldown_until_ms_ = 0;
};

/** @brief SparkBot BMI270 采集适配器。 */
class SparkBotImu final {
   public:
    using ShakeCallback = std::function<void()>;

    /** @brief 创建未启动的 BMI270 采集适配器。 */
    SparkBotImu();
    /** @brief 停止采集并释放适配器资源。 */
    ~SparkBotImu();

    /** @brief 禁止复制 BMI270 采集适配器。 */
    SparkBotImu(const SparkBotImu&) = delete;
    /** @brief 禁止复制赋值 BMI270 采集适配器。 */
    SparkBotImu& operator=(const SparkBotImu&) = delete;

    /**
     * @brief 启动 BMI270 采集任务；host 构建返回明确不可用。
     * @param callback 检测到有效摇晃时调用的回调。
     * @return 启动结果。
     */
    [[nodiscard]] Status Start(ShakeCallback callback);
    /** @brief 请求任务停止并释放 BMI270 device handle，不释放共享 I2C 总线。 */
    void Stop();
    /**
     * @brief 返回采集任务是否已启动。
     * @return 采集任务是否正在运行。
     */
    [[nodiscard]] bool running() const { return running_.load(); }

   private:
    /** @brief ESP-IDF 平台实现的私有状态。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

}  // namespace voicelife::board_esp
