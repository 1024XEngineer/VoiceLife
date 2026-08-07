#include <memory>
#include <utility>

#include "support/test_support.h"
#include "voicelife/voice/voice_session.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

class FakeInput final : public voicelife::voice::AudioInputPort {
   public:
    void SetAudioSink(voicelife::voice::AudioFrameSink sink) override { audio_sink_ = std::move(sink); }
    Status Open(const voicelife::voice::AudioFormat& format) override {
        ++opens;
        opened_format = format;
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

    Status EmitCapture(voicelife::voice::AudioFrame frame) {
        return audio_sink_ ? audio_sink_(std::move(frame))
                           : Status::Error(ErrorCode::kUnavailable, "音频采集回调未绑定");
    }

    Status open_result = Status::Ok();
    Status start_result = Status::Ok();
    Status stop_result = Status::Ok();
    voicelife::voice::AudioFormat opened_format;
    int opens = 0;
    int starts = 0;
    int stops = 0;
    int closes = 0;

   private:
    voicelife::voice::AudioFrameSink audio_sink_;
};

class FakeOutput final : public voicelife::voice::AudioOutputPort {
   public:
    Status Open(const voicelife::voice::AudioFormat& format) override {
        ++opens;
        opened_format = format;
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
    voicelife::voice::AudioFormat opened_format;
    int opens = 0;
    int pushes = 0;
    int flushes = 0;
    int closes = 0;
};

class FakeProvider final : public voicelife::voice::SpeechProviderAdapter {
   public:
    void SetAudioSink(voicelife::voice::AudioFrameSink sink) override { audio_sink_ = std::move(sink); }
    void SetGeneration(uint64_t generation) override { generation_ = generation; }
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
    Status SendAudio(const voicelife::voice::AudioFrame& frame) override {
        ++audio_frames;
        last_audio_frame = frame;
        return send_result;
    }
    Status Abort(std::string_view) override {
        ++aborts;
        generation_at_abort = generation_;
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
    voicelife::Result<voicelife::voice::VoiceAudioFormats> audio_formats() const override {
        return voicelife::Result<voicelife::voice::VoiceAudioFormats>::Success(formats);
    }
    const voicelife::voice::CapabilityProfile& capabilities() const override { return profile; }

    void Emit(voicelife::voice::VoiceEvent event) {
        if (sink_) {
            sink_(event);
        }
    }
    Status EmitAudio(voicelife::voice::AudioFrame frame) {
        return audio_sink_ ? audio_sink_(std::move(frame)) : Status::Error(ErrorCode::kUnavailable, "音频回调未绑定");
    }

    voicelife::voice::CapabilityProfile profile{"fake", {"streaming-asr", "tts", "cancel-generation"}};
    voicelife::voice::VoiceEventSink sink_;
    voicelife::voice::AudioFrameSink audio_sink_;
    voicelife::voice::AudioFrame last_audio_frame;
    voicelife::voice::VoiceAudioFormats formats;
    uint64_t generation_ = 0;
    uint64_t generation_at_abort = 0;
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
    Check(registry
              .Register("fake-registry", voicelife::voice::CapabilityProfile{"fake-registry", {"tts"}},
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
    Check(provider.EmitAudio(Frame(generation, 0)).ok() && output.pushes == 1, "Provider 下行音频应通过会话输出端口");
    auto mismatched_playback = Frame(generation, 0);
    mismatched_playback.format.channels = 2;
    Check(provider.EmitAudio(std::move(mismatched_playback)).code == ErrorCode::kInvalidArgument && output.pushes == 1,
          "下行音频格式变化必须拒绝");
    Check(session.BeginCapture().ok(), "ready 会话应开始采集");
    Check(session.SubmitAudio(Frame(generation, 0)).ok(), "当前 generation 的首帧应发送");
    Check(input.EmitCapture(Frame(0, 0)).ok() && provider.audio_frames == 2, "输入端口采集回调应转发为上行音频");
    Check(provider.last_audio_frame.generation == generation && provider.last_audio_frame.sequence == 1,
          "会话应为输入回调补齐当前 generation 和连续序号");
    auto mismatched_format = Frame(generation, 1);
    mismatched_format.format.sample_rate_hz = 8000;
    Check(session.SubmitAudio(mismatched_format).code == ErrorCode::kInvalidArgument,
          "采样率与会话不一致的音频帧必须拒绝");
    auto mismatched_codec = Frame(generation, 1);
    mismatched_codec.format.codec = voicelife::voice::AudioCodec::kOpus;
    Check(session.SubmitAudio(mismatched_codec).code == ErrorCode::kInvalidArgument,
          "编码与会话不一致的音频帧必须拒绝");
    Check(session.SubmitAudio(Frame(generation, 3)).code == ErrorCode::kConflict, "跳号音频帧必须拒绝");
    Check(session.SubmitAudio(Frame(generation - 1, 1)).code == ErrorCode::kInvalidArgument,
          "旧 generation 音频帧必须拒绝");
    Check(session.EndCapture().ok(), "采集应可正常结束");
    Check(input.EmitCapture(Frame(0, 0)).code == ErrorCode::kUnavailable, "结束采集后迟到的输入帧必须拒绝");
    Check(session.Speak("测试播报").ok(), "ready 会话应允许播报");
    voicelife::voice::VoiceEvent tts_started;
    tts_started.kind = voicelife::voice::VoiceEventKind::kTtsStarted;
    tts_started.generation = generation;
    provider.Emit(tts_started);
    Check(session.state() == voicelife::voice::VoiceSessionState::kSpeaking, "TTS start 应进入 speaking");
    Check(session.Interrupt().ok(), "播报应支持打断");
    Check(session.generation() != generation && provider.generation_ == session.generation() &&
              provider.generation_at_abort == session.generation() && output.flushes == 1,
          "打断应刷新播放并让 Provider 切换到新 generation");
    Check(provider.EmitAudio(Frame(generation, 1)).code == ErrorCode::kInvalidArgument && output.pushes == 1,
          "打断后迟到的旧 generation 音频不得重新进入播放队列");
    provider.Emit(voicelife::voice::VoiceEvent{});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady,
          "缺少 generation 的迟到 Provider 事件不能改变新会话状态");
    Check(session.Stop().ok() && session.state() == voicelife::voice::VoiceSessionState::kStopped,
          "停止应关闭 Provider 和音频端口");
    Check(input.EmitCapture(Frame(0, 0)).code == ErrorCode::kUnavailable,
          "停止会话应清理输入回调，避免资源关闭后的迟到帧");
    Check(evidence_count >= 4, "会话生命周期应产出可关联的证据事件");

    FakeInput bad_input;
    bad_input.open_result = Status::Error(ErrorCode::kUnavailable, "麦克风不可用");
    FakeOutput unused_output;
    FakeProvider unused_provider;
    voicelife::voice::VoiceSession failed(bad_input, unused_output, unused_provider);
    Check(failed.Start(Config()).code == ErrorCode::kUnavailable, "输入端口失败应向上传播");
    Check(unused_provider.connects == 1 && unused_provider.disconnects == 1,
          "音频协商后输入端口失败必须回滚 Provider 连接");

    FakeInput connect_failure_input;
    FakeOutput connect_failure_output;
    FakeProvider connect_failure_provider;
    connect_failure_provider.connect_result = Status::Error(ErrorCode::kUnavailable, "Provider 连接失败");
    voicelife::voice::VoiceSession connect_failure(connect_failure_input, connect_failure_output,
                                                   connect_failure_provider);
    Check(connect_failure.Start(Config()).code == ErrorCode::kUnavailable &&
              connect_failure_provider.disconnects == 1 &&
              connect_failure.state() == voicelife::voice::VoiceSessionState::kFailed,
          "Provider 连接失败后会话必须执行断开回滚并进入 failed");

    FakeInput capture_failure_input;
    FakeOutput capture_failure_output;
    FakeProvider capture_failure_provider;
    capture_failure_input.start_result = Status::Error(ErrorCode::kUnavailable, "采集启动失败");
    capture_failure_provider.stop_result = Status::Error(ErrorCode::kUnavailable, "远端采集回滚失败");
    voicelife::voice::VoiceSession capture_failure(capture_failure_input, capture_failure_output,
                                                   capture_failure_provider);
    Check(capture_failure.Start(Config()).ok(), "回滚失败用例应先启动会话");
    Check(capture_failure.BeginCapture().code == ErrorCode::kUnavailable &&
              capture_failure.state() == voicelife::voice::VoiceSessionState::kFailed,
          "本地采集启动和远端回滚都失败时会话必须进入 failed");

    FakeInput stop_capture_failure_input;
    FakeOutput stop_capture_failure_output;
    FakeProvider stop_capture_failure_provider;
    voicelife::voice::VoiceSession stop_capture_failure(stop_capture_failure_input, stop_capture_failure_output,
                                                        stop_capture_failure_provider);
    Check(stop_capture_failure.Start(Config()).ok() && stop_capture_failure.BeginCapture().ok(),
          "停止采集失败用例应先进入 capturing");
    stop_capture_failure_provider.stop_result = Status::Error(ErrorCode::kUnavailable, "远端停止失败");
    Check(stop_capture_failure.EndCapture().code == ErrorCode::kUnavailable &&
              stop_capture_failure.state() == voicelife::voice::VoiceSessionState::kFailed,
          "本地已停止而远端停止失败时不得继续保持 capturing");

    FakeInput disconnect_failure_input;
    FakeOutput disconnect_failure_output;
    FakeProvider disconnect_failure_provider;
    disconnect_failure_provider.disconnect_result = Status::Error(ErrorCode::kUnavailable, "Provider 断开失败");
    voicelife::voice::VoiceSession disconnect_failure(disconnect_failure_input, disconnect_failure_output,
                                                      disconnect_failure_provider);
    Check(disconnect_failure.Start(Config()).ok(), "断开失败用例应先启动会话");
    Check(disconnect_failure.Stop().code == ErrorCode::kUnavailable &&
              disconnect_failure.state() == voicelife::voice::VoiceSessionState::kFailed,
          "Provider 断开失败时会话不得伪装为 stopped");

    FakeInput speak_input;
    FakeOutput speak_output;
    FakeProvider speak_provider;
    speak_provider.speak_result = Status::Error(ErrorCode::kUnavailable, "TTS 不可用");
    voicelife::voice::VoiceSession speak_failure(speak_input, speak_output, speak_provider);
    Check(speak_failure.Start(Config()).ok(), "TTS 失败前会话应已启动");
    Check(speak_failure.Speak("失败测试").code == ErrorCode::kUnavailable &&
              speak_failure.state() == voicelife::voice::VoiceSessionState::kReady,
          "TTS 失败不得卡在 speaking 状态");

    FakeInput negotiated_input;
    FakeOutput negotiated_output;
    FakeProvider negotiated_provider;
    negotiated_provider.formats.capture = Config().audio;
    negotiated_provider.formats.playback = Config().audio;
    negotiated_provider.formats.playback.sample_rate_hz = 24000;
    negotiated_provider.formats.playback.frame_duration_ms = 60;
    voicelife::voice::VoiceSession negotiated_session(negotiated_input, negotiated_output, negotiated_provider);
    Check(negotiated_session.Start(Config()).ok(), "Provider 协商不同下行格式后会话应可启动");
    Check(negotiated_input.opened_format.sample_rate_hz == 16000 &&
              negotiated_input.opened_format.frame_duration_ms == 20,
          "输入端口必须使用设备请求的上行格式");
    Check(negotiated_output.opened_format.sample_rate_hz == 24000 &&
              negotiated_output.opened_format.frame_duration_ms == 60,
          "输出端口必须在 Provider hello 后使用协商的下行格式");
    auto negotiated_playback = Frame(negotiated_session.generation(), 0);
    negotiated_playback.format = negotiated_provider.formats.playback;
    Check(negotiated_provider.EmitAudio(std::move(negotiated_playback)).ok() && negotiated_output.pushes == 1,
          "协商后的 24 kHz 下行音频应进入输出端口");
    const uint64_t speaking_generation = negotiated_session.generation();
    Check(negotiated_session.Speak("测试打断").ok(), "协商会话应可播报");
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                                          .generation = speaking_generation,
                                                          .text = {},
                                                          .aborted = false});
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                                          .generation = speaking_generation,
                                                          .text = {},
                                                          .aborted = true});
    Check(negotiated_output.flushes == 1 && negotiated_session.generation() != speaking_generation,
          "服务端 abort 必须立即清空播放缓冲并失效旧代次");
    const uint64_t disconnected_generation = negotiated_session.generation();
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kDisconnected,
                                                          .generation = disconnected_generation,
                                                          .text = {},
                                                          .aborted = false});
    Check(negotiated_session.state() == voicelife::voice::VoiceSessionState::kStarting &&
              negotiated_session.generation() != disconnected_generation,
          "断线必须进入等待重连状态并失效旧代次");
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kConnected,
                                                          .generation = negotiated_session.generation(),
                                                          .text = {},
                                                          .aborted = false});
    Check(negotiated_session.state() == voicelife::voice::VoiceSessionState::kReady,
          "重连 hello 完成后会话应回到 ready");

    // SpeechProviderRegistry 错误路径覆盖。
    Check(registry.Register("", voicelife::voice::CapabilityProfile{}, nullptr).code == ErrorCode::kInvalidArgument,
          "空 Provider ID 与空工厂必须拒绝");
    Check(registry.Register("mismatch", voicelife::voice::CapabilityProfile{"other", {}},
                            []() { return std::unique_ptr<FakeProvider>(); })
                  .code == ErrorCode::kInvalidArgument,
          "Profile ID 与注册 ID 不一致必须拒绝");
    Check(registry.Create("no-such-provider", {}).status.code == ErrorCode::kNotFound,
          "未注册 Provider 必须返回 NotFound");
    Check(registry.Create("fake-registry", {"aec", "vad"}).status.code == ErrorCode::kUnavailable,
          "缺少任一必需能力必须拒绝");
    Check(registry.Register("fake-registry", voicelife::voice::CapabilityProfile{"fake-registry", {"tts"}},
                            []() { return std::make_unique<FakeProvider>(); })
                  .code == ErrorCode::kAlreadyExists,
          "重复注册同一 Provider ID 必须返回 AlreadyExists");

    return 0;
}
