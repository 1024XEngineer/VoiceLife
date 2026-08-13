#pragma once

#include <memory>

#include "voicelife/voice/wake_gate_audio_input.h"

namespace voicelife::audio_esp {

/** ESP-SR WakeNet 的板级适配器，模型数据必须来自受控 assets mmap。 */
class EspWakeNetDetector final : public voice::LocalWakeDetectorPort {
   public:
    explicit EspWakeNetDetector(const void* model_root);
    ~EspWakeNetDetector() override;

    EspWakeNetDetector(const EspWakeNetDetector&) = delete;
    EspWakeNetDetector& operator=(const EspWakeNetDetector&) = delete;

    Status Start(WakeSink sink) override;
    Status Stop() override;
    Status Submit(const voice::AudioFrame& frame) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::audio_esp
