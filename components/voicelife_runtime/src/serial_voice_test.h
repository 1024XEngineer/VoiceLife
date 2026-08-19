#pragma once

#include <functional>
#include <memory>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::runtime {

/** Callbacks that keep the serial harness outside the interaction state owner. */
struct SerialVoiceTestCallbacks {
    std::function<Status()> begin_turn;
    std::function<Status(voice::AudioFrame)> submit_pcm;
    std::function<Status()> end_turn;
};

/**
 * Test-only USB serial PCM reader.
 *
 * Protocol: `VLVT`, version 1, kind, little-endian payload length. Kinds are
 * begin=1 (empty), pcm=2 (exactly one 20 ms PCM frame), and end=3 (empty).
 */
class SerialVoiceTest final {
   public:
    explicit SerialVoiceTest(SerialVoiceTestCallbacks callbacks);
    ~SerialVoiceTest();

    SerialVoiceTest(const SerialVoiceTest&) = delete;
    SerialVoiceTest& operator=(const SerialVoiceTest&) = delete;

    Status Start();
    void Stop();

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::runtime
