#pragma once

#include <memory>

#include "voicelife/voice/wake_gate_audio_input.h"

namespace voicelife::audio_esp {

/** ESP-SR WakeNet 的板级适配器，模型数据必须来自受控 assets mmap。 */
class EspWakeNetDetector final : public voice::LocalWakeDetectorPort {
   public:
    /** @brief 以受控模型根指针创建 WakeNet 检测器。 @param model_root 模型数据根指针。 */
    explicit EspWakeNetDetector(const void* model_root);
    /** @brief 销毁检测器及其 ESP-SR 资源。 */
    ~EspWakeNetDetector() override;

    /** @brief 禁止复制检测器。 @param 复制源检测器。 */
    EspWakeNetDetector(const EspWakeNetDetector&) = delete;
    /** @brief 禁止复制赋值检测器。 @param 复制源检测器。 @return 本对象引用。 */
    EspWakeNetDetector& operator=(const EspWakeNetDetector&) = delete;

    /** @brief 启动 WakeNet 检测。 @param sink 唤醒命中回调。 @return 启动结果。 */
    Status Start(WakeSink sink) override;
    /** @brief 停止 WakeNet 检测。 @return 停止结果。 */
    Status Stop() override;
    /** @brief 提交一帧待检测的音频。 @param frame PCM 音频帧。 @return 提交结果。 */
    Status Submit(const voice::AudioFrame& frame) override;

   private:
    /** @brief ESP-SR 具体资源的私有实现。 */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
