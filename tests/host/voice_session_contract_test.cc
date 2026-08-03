#include "voicelife/voice/voice_session.h"

#include <memory>

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

class FakeInput final : public voicelife::voice::AudioInputPort {
   public:
    Status Open(const voicelife::voice::AudioFormat&) override {
        ++opens;
        return open_result;
    }
    Status StartCapture(voicelife::voice::VoiceMode) override {
        ++starts;
        return start_result;
    }
    Status StopCapture() override {
        ++stops;
        return stop_result;
    }
    void Close() override { ++closes; }

    Status open_result = Status::Ok();
    Status start_result = Status::Ok();
    Status stop_result = Status::Ok();
    int opens = 0;
    int starts = 0;
    int stops = 0;
    int closes = 0;
};

class FakeOutput final : public voicelife::voice::AudioOutputPort {
   public:
    Status Open(const voicelife::voice::AudioFormat&) override {
        ++opens;
        return open_result;
    }
    Status Push(const voicelife::voice::AudioFrame&) override {
        ++pushes;
        return push_result;
    }
    Status Flush() override {
        ++flushes;
        return flush_result;
    }
    void Close() override { ++closes; }

    Status open_result = Status::Ok();
    Status push_result = Status::Ok();
    Status flush_result = Status::Ok();
    int opens = 0;
    int pushes = 0;
    int flushes = 0;
    int closes = 0;
};

class FakeProvider final : public voicelife::voice::SpeechProviderAdapter {
   public:
    Status Connect(const voicelife::voice::VoiceSessionConfig&, voicelife::voice::VoiceEventSink sink) override {
        ++connects;
        sink_ = std::move(sink);
        return connect_result;
    }
    Status StartCapture(voicelife::voice::VoiceMode) override {
        ++starts;
        return start_result;
    }
    Status StopCapture() override {
        ++stops;
        return stop_result;
    }
    Status SendAudio(const voicelife::voice::AudioFrame&) override {
        ++audio_frames;
        return send_result;
    }
    Status Abort(std::string_view) override {
        ++aborts;
        return abort_result;
    }
    Status Speak(std::string_view) override {
        ++speaks;
        return speak_result;
    }
    Status Disconnect() override {
        ++disconnects;
        return disconnect_result;
    }
    const voicelife::voice::CapabilityProfile& capabilities() const override { return profile; }

    void Emit(voicelife::voice::VoiceEvent event) {
        if (sink_) {
            sink_(event);
        }
    }

    voicelife::voice::CapabilityProfile profile{"fake", {"streaming-asr", "tts", "cancel-generation"}};
    voicelife::voice::VoiceEventSink sink_;
    Status connect_result = Status::Ok();
    Status start_result = Status::Ok();
    Status stop_result = Status::Ok();
    Status send_result = Status::Ok();
    Status abort_result = Status::Ok();
    Status speak_result = Status::Ok();
    Status disconnect_result = Status::Ok();
    int connects = 0;
    int starts = 0;
    int stops = 0;
    int audio_frames = 0;
    int aborts = 0;
    int speaks = 0;
    int disconnects = 0;
};

voicelife::voice::VoiceSessionConfig Config() {
    voicelife::voice::VoiceSessionConfig config;
    config.session_id = "test-session";
    config.provider_id = "fake";
    config.audio.codec = voicelife::voice::AudioCodec::kPcmS16Le;
    return config;
}

voicelife::voice::AudioFrame Frame(uint64_t generation, uint64_t sequence) {
    voicelife::voice::AudioFrame frame;
    frame.generation = generation;
    frame.sequence = sequence;
    frame.payload = {1, 2, 3};
    return frame;
}

}  // namespace

int main() {
    auto& registry = voicelife::voice::SpeechProviderRegistry::Instance();
    Check(registry.Register(
              "fake-registry", voicelife::voice::CapabilityProfile{"fake-registry", {"tts"}},
              []() { return std::make_unique<FakeProvider>(); })
              .ok(),
          "Provider 工厂应可注册");
    auto created = registry.Create("fake-registry", {"tts"});
    Check(created.ok() && created.value.has_value() && created.value.value() != nullptr,
          "注册 Provider 应可按能力创建");
    auto missing_capability = registry.Create("fake-registry", {"aec"});
    Check(missing_capability.status.code == ErrorCode::kUnavailable, "缺少能力时不能静默降级");
    FakeInput input;
    FakeOutput output;
    FakeProvider provider;
    int evidence_count = 0;
    voicelife::voice::VoiceSession session(
        input, output, provider, [&evidence_count](const voicelife::voice::VoiceEvidence&) { ++evidence_count; });

    Check(session.Start(Config()).ok(), "合法配置应启动语音会话");
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady, "启动后应进入 ready");
    const uint64_t generation = session.generation();
    Check(session.BeginCapture().ok(), "ready 会话应开始采集");
    Check(session.SubmitAudio(Frame(generation, 0)).ok(), "当前 generation 的首帧应发送");
    Check(session.SubmitAudio(Frame(generation, 2)).code == ErrorCode::kConflict, "跳号音频帧必须拒绝");
    Check(session.SubmitAudio(Frame(generation - 1, 1)).code == ErrorCode::kInvalidArgument,
          "旧 generation 音频帧必须拒绝");
    Check(session.EndCapture().ok(), "采集应可正常结束");
    Check(session.Speak("测试播报").ok(), "ready 会话应允许播报");
    voicelife::voice::VoiceEvent tts_started;
    tts_started.kind = voicelife::voice::VoiceEventKind::kTtsStarted;
    tts_started.generation = generation;
    provider.Emit(tts_started);
    Check(session.state() == voicelife::voice::VoiceSessionState::kSpeaking, "TTS start 应进入 speaking");
    Check(session.Interrupt().ok(), "播报应支持打断");
    Check(session.generation() != generation && output.flushes == 1, "打断应刷新播放并失效旧 generation");
    Check(session.Stop().ok() && session.state() == voicelife::voice::VoiceSessionState::kStopped,
          "停止应关闭 Provider 和音频端口");
    Check(evidence_count >= 4, "会话生命周期应产出可关联的证据事件");

    FakeInput bad_input;
    bad_input.open_result = Status::Error(ErrorCode::kUnavailable, "麦克风不可用");
    FakeOutput unused_output;
    FakeProvider unused_provider;
    voicelife::voice::VoiceSession failed(bad_input, unused_output, unused_provider);
    Check(failed.Start(Config()).code == ErrorCode::kUnavailable, "输入端口失败应向上传播");
    Check(unused_provider.connects == 0, "输入端口失败后不能连接 Provider");

    FakeInput speak_input;
    FakeOutput speak_output;
    FakeProvider speak_provider;
    speak_provider.speak_result = Status::Error(ErrorCode::kUnavailable, "TTS 不可用");
    voicelife::voice::VoiceSession speak_failure(speak_input, speak_output, speak_provider);
    Check(speak_failure.Start(Config()).ok(), "TTS 失败前会话应已启动");
    Check(speak_failure.Speak("失败测试").code == ErrorCode::kUnavailable &&
              speak_failure.state() == voicelife::voice::VoiceSessionState::kReady,
          "TTS 失败不得卡在 speaking 状态");

    return 0;
}
