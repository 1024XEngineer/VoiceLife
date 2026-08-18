#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/voice/audio_aec.h"

namespace voicelife::audio_esp {

/**
 * @brief ESP-SR AEC 适配器的生命周期骨架。
 *
 * 当前版本只做能力/硬件 reference 校验并明确返回 unavailable；真实 AFE
 * handle、ProcessingTask 和 fetch_with_delay() 会在 ESP-SR 适配提交中加入，
 * 不会进入 VoiceSession 或公共头文件。
 */
class EspAecService final : public voice::AudioAecService {
   public:
    /** @brief 使用板级 Profile 创建 AEC 适配器。 @param profile 已验证的音频硬件描述。 */
    explicit EspAecService(AudioBoardProfile profile);
    /** @brief 释放适配器持有的资源。 */
    ~EspAecService() override = default;

    /** @brief 返回 Profile 允许声明的能力。 @return 能力快照。 */
    [[nodiscard]] voice::AudioAecCapabilities capabilities() const override;
    /** @brief 验证 AEC 启动前置条件。 @param config AEC 配置。 @param cleaned_sink 输出回调。 @return 启动结果。 */
    Status Start(const voice::AudioAecConfig& config, voice::AudioFrameSink cleaned_sink) override;
    /** @brief 提交采集帧。 @param frame PCM 采集帧。 @return 接收结果。 */
    Status SubmitCapture(voice::AudioFrame frame) override;
    /** @brief 提交播放 reference。 @param frame PCM reference 帧。 @return 接收结果。 */
    Status SubmitPlaybackReference(voice::AudioFrame frame) override;
    /** @brief 请求切换 AEC。 @param enabled 目标启用状态。 @return 设置结果。 */
    Status SetEnabled(bool enabled) override;
    /** @brief 切换代次并清理旧状态。 @param generation 新代次。 @return 重置结果。 */
    Status Reset(uint64_t generation) override;
    /** @brief 停止 AEC 适配器。 @return 停止结果。 */
    Status Stop() override;
    /** @brief 获取适配器统计。 @return 统计快照。 */
    [[nodiscard]] voice::AudioAecStats stats() const override;

   private:
    AudioBoardProfile profile_;
    mutable std::mutex mutex_;
    std::optional<voice::AudioAecConfig> config_;
    voice::AudioFrameSink cleaned_sink_;
    voice::AudioAecStats stats_;
    bool enabled_ = false;
};

}  // namespace voicelife::audio_esp
