#pragma once

#include "voicelife/board_esp/sparkbot_profile.h"

namespace voicelife::board_esp {

/** @brief GPIO46 两个消费者的共享线状态。 */
struct SharedPowerState {
    /** @brief 背光消费者是否请求启用。 */
    bool backlight_enabled = false;
    /** @brief 音频输出消费者是否请求启用。 */
    bool audio_output_enabled = false;
    /** @brief 仲裁后的物理线电平。 */
    bool line_level = false;
};

/**
 * @brief 集中仲裁 SparkBot 的 GPIO46 功放/背光共享线。
 *
 * 该类型只维护逻辑状态，不直接持有 GPIO 或 ESP-IDF 句柄。设备驱动
 * 应在自己的任务中读取 line_level() 后完成实际 GPIO 写入。
 */
class Gpio46PowerArbiter final {
   public:
    /**
     * @brief 创建共享电源仲裁器。
     * @param profile 共享线硬件 Profile。
     */
    explicit Gpio46PowerArbiter(SharedPowerProfile profile);

    /**
     * @brief 校验共享线配置。
     * @return 配置合法返回 Ok。
     */
    [[nodiscard]] Status Validate() const;

    /**
     * @brief 更新背光消费者请求。
     * @param enabled 是否请求背光启用。
     * @return 更新结果。
     */
    Status SetBacklightEnabled(bool enabled);

    /**
     * @brief 更新音频输出消费者请求。
     * @param enabled 是否请求音频输出启用。
     * @return 更新结果。
     */
    Status SetAudioOutputEnabled(bool enabled);

    /** @brief 返回当前两个消费者的请求和线电平。 @return 共享线状态。 */
    [[nodiscard]] SharedPowerState state() const;

    /** @brief 返回是否应向 GPIO 写入启用电平。 @return 启用返回 true。 */
    [[nodiscard]] bool line_enabled() const;

    /** @brief 返回是否处于待机安全状态。 @return 两个消费者都关闭时返回 true。 */
    [[nodiscard]] bool idle_safe() const;

   private:
    /** @brief 共享线硬件参数。 */
    SharedPowerProfile profile_;
    /** @brief 背光消费者状态。 */
    bool backlight_enabled_ = false;
    /** @brief 音频消费者状态。 */
    bool audio_output_enabled_ = false;
};

}  // namespace voicelife::board_esp
