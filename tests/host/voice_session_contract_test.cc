#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "support/test_support.h"
#include "voicelife/voice/voice_session.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

class FakeInput final : public voicelife::voice::AudioInputPort {
   public:
    explicit FakeInput(std::vector<char>* close_order = nullptr) : close_order_(close_order) {}
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
    void Close() override {
        ++closes;
        if (close_order_ != nullptr) close_order_->push_back('i');
    }

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
    std::vector<char>* close_order_ = nullptr;
};

class FakeOutput final : public voicelife::voice::AudioOutputPort {
   public:
    explicit FakeOutput(std::vector<char>* close_order = nullptr) : close_order_(close_order) {}
    Status Open(const voicelife::voice::AudioFormat& format) override {
        ++opens;
        opened_format = format;
        return open_result;
    }
    Status Push(voicelife::voice::AudioFrame frame) override {
        ++pushes;
        last_frame = std::move(frame);
        return push_result;
    }
    Status Flush() override {
        ++flushes;
        return flush_result;
    }
    bool IsIdle() const override { return true; }
    void Close() override {
        ++closes;
        if (close_order_ != nullptr) close_order_->push_back('o');
    }

    Status open_result = Status::Ok();
    Status push_result = Status::Ok();
    Status flush_result = Status::Ok();
    voicelife::voice::AudioFormat opened_format;
    int opens = 0;
    int pushes = 0;
    int flushes = 0;
    int closes = 0;
    voicelife::voice::AudioFrame last_frame;

   private:
    std::vector<char>* close_order_ = nullptr;
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
        calls.push_back("listen.start");
        return start_result;
    }
    Status StopCapture() override {
        ++stops;
        return stop_result;
    }
    Status SendAudio(voicelife::voice::AudioFrame frame) override {
        ++audio_frames;
        last_audio_frame = std::move(frame);
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
    Status NotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response = {}) override {
        ++wake_notifications;
        calls.push_back("listen.detect");
        last_wake_word = std::string(wake_word);
        last_wake_response = std::string(text_response);
        return wake_notification_result;
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
    Status wake_notification_result = Status::Ok();
    Status disconnect_result = Status::Ok();
    int connects = 0;
    int starts = 0;
    int stops = 0;
    int audio_frames = 0;
    int aborts = 0;
    int speaks = 0;
    int wake_notifications = 0;
    int disconnects = 0;
    std::vector<std::string> calls;
    std::string last_wake_word;
    std::string last_wake_response;
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

voicelife::voice::AudioFrame SpeechFrame(uint64_t generation, uint64_t sequence) {
    auto frame = Frame(generation, sequence);
    frame.payload.assign(640, 0);
    for (std::size_t index = 0; index < frame.payload.size(); index += 2) {
        frame.payload[index + 1] = 0x10;  // 4096 S16LE: definitively above VAD entry threshold.
    }
    return frame;
}

voicelife::voice::AudioFrame SilenceFrame(uint64_t generation, uint64_t sequence) {
    auto frame = Frame(generation, sequence);
    frame.payload.assign(640, 0);
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
    std::vector<char> close_order;
    FakeInput input(&close_order);
    FakeOutput output(&close_order);
    FakeProvider provider;
    int evidence_count = 0;
    std::vector<voicelife::voice::VoiceEvidence> evidence;
    voicelife::voice::VoiceSession session(input, output, provider,
                                           [&evidence_count, &evidence](const voicelife::voice::VoiceEvidence& item) {
                                               ++evidence_count;
                                               evidence.push_back(item);
                                           });

    auto invalid_vad_config = Config();
    invalid_vad_config.vad_silence_ms = 0;
    Check(session.Start(invalid_vad_config).code == ErrorCode::kInvalidArgument, "零毫秒 VAD 端点窗口必须在启动前拒绝");
    Check(session.Start(Config()).ok(), "合法配置应启动语音会话");
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady, "启动后应进入 ready");
    Check(session.NotifyLocalWakeWord("你好牛牛", "收到！").ok() && provider.wake_notifications == 1 &&
              provider.last_wake_word == "你好牛牛" && provider.last_wake_response == "收到！",
          "普通本地唤醒必须通过 Provider 请求受控确认 TTS");
    provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                               .generation = session.generation(),
                                               .text = {},
                                               .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kSpeaking,
          "唤醒确认的真实 TTS start 才能进入 speaking");
    provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                               .generation = session.generation(),
                                               .text = {},
                                               .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady,
          "唤醒确认 TTS 结束后会话必须允许开始真实聆听");
    session.ReportToolCallStarted();
    session.ReportToolResult("event=创建会议", true);
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady && provider.audio_frames == 0 &&
              output.pushes == 0,
          "MCP 语义证据不得伪造音频、改变会话状态或绕过 VoiceSession");
    Check(evidence.size() >= 3 && evidence[evidence.size() - 2].event == "mcp_tool_started" &&
              evidence.back().event == "mcp_tool_result" && evidence.back().detail == "event=创建会议",
          "MCP worker 只能通过 VoiceSession 的受控 evidence 出口回注结果");
    const uint64_t generation = session.generation();
    // 空闲（kReady）收到服务端残留 TTS start 必须忽略，不进入播报。
    provider.Emit(voicelife::voice::VoiceEvent{
        .kind = voicelife::voice::VoiceEventKind::kTtsStarted, .generation = generation, .text = {}, .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady, "空闲时收到残留 TTS start 不得进入播报状态");
    Check(provider.EmitAudio(Frame(generation, 0)).code == ErrorCode::kUnavailable && output.pushes == 0,
          "空闲时残留 TTS 二进制帧不得播放");
    // 正常流程：进入聆听并收到有效 STT 后，TTS start 才被接受。
    Check(session.BeginCapture().ok(), "ready 会话应开始采集");
    provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kAsrText,
                                               .generation = generation,
                                               .text = "你好",
                                               .aborted = false});
    provider.Emit(voicelife::voice::VoiceEvent{
        .kind = voicelife::voice::VoiceEventKind::kTtsStarted, .generation = generation, .text = {}, .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kSpeaking,
          "收到有效 STT 后 TTS start 应进入播报状态");
    auto first_playback = Frame(generation, 0);
    const auto* first_playback_data = first_playback.payload.data();
    Check(provider.EmitAudio(std::move(first_playback)).ok() && output.pushes == 1 &&
              output.last_frame.payload.data() == first_playback_data,
          "TTS start 后的下行音频应通过会话输出端口");
    Check(output.last_frame.payload.data() == first_playback_data,
          "会话到播放端口必须移动 PCM 负载，不能复制每个下行帧");
    Check(!evidence.empty() && evidence.back().event == "tts_first_audio",
          "每段 TTS 首个成功播放帧必须提供无内容的时延证据");
    const std::size_t evidence_before_late_asr = evidence.size();
    provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kAsrText,
                                               .generation = generation,
                                               .text = "迟到的上一段识别",
                                               .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kSpeaking &&
              evidence.size() == evidence_before_late_asr + 1 && evidence.back().event == "stale_event_dropped",
          "播报中的迟到 STT 必须在 VoiceSession 丢弃，不能重新驱动交互状态");
    const int input_stops_before_late_end = input.stops;
    const int provider_stops_before_late_end = provider.stops;
    Check(session.EndCapture().ok() && input.stops == input_stops_before_late_end &&
              provider.stops == provider_stops_before_late_end,
          "TTS 已关闭输入后，迟到的结束采集必须幂等且不得停止正在播报的会话");
    provider.Emit(voicelife::voice::VoiceEvent{
        .kind = voicelife::voice::VoiceEventKind::kTtsStopped, .generation = generation, .text = {}, .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady, "TTS stop 后回到 ready");
    auto mismatched_playback = Frame(generation, 0);
    mismatched_playback.format.channels = 2;
    Check(provider.EmitAudio(std::move(mismatched_playback)).code == ErrorCode::kUnavailable && output.pushes == 1,
          "非播报状态的下行音频必须拒绝");
    Check(session.BeginCapture().ok(), "ready 会话应开始采集");
    Check(session.SubmitAudio(Frame(generation, 0)).ok(), "当前 generation 的首帧应发送");
    auto captured = SpeechFrame(0, 0);
    const auto* captured_data = captured.payload.data();
    Check(input.EmitCapture(std::move(captured)).ok() && provider.audio_frames == 2 &&
              provider.last_audio_frame.payload.data() == captured_data,
          "输入端口采集回调应转发为上行音频");
    Check(
        std::count_if(evidence.begin(), evidence.end(),
                      [](const voicelife::voice::VoiceEvidence& item) { return item.event == "speech_started"; }) == 1,
        "每轮首个有效语音帧必须只上报一次 speech_started");
    Check(provider.last_audio_frame.payload.data() == captured_data,
          "采集到 Provider 的上行 PCM 负载必须移动，不能复制每个音频帧");
    Check(provider.last_audio_frame.generation == generation && provider.last_audio_frame.sequence == 1,
          "会话应为输入回调补齐当前 generation 和连续序号");

    // A conversational turn may contain a brief natural pause. The default
    // endpoint must not fire before speech resumes; the final silence still
    // has to end the turn once it exceeds the configured window.
    FakeInput vad_input;
    FakeOutput vad_output;
    FakeProvider vad_provider;
    std::vector<voicelife::voice::VoiceEvidence> vad_evidence;
    voicelife::voice::VoiceSession vad_session(
        vad_input, vad_output, vad_provider,
        [&vad_evidence](const voicelife::voice::VoiceEvidence& item) { vad_evidence.push_back(item); });
    auto short_vad_config = Config();
    short_vad_config.vad_silence_ms = 80;
    Check(vad_session.Start(short_vad_config).ok() && vad_session.BeginCapture().ok(), "VAD 回归用例应启动采集");
    Check(vad_input.EmitCapture(SpeechFrame(0, 0)).ok(), "VAD 回归用例应先发送语音帧");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(vad_input.EmitCapture(SilenceFrame(0, 0)).ok() &&
              std::none_of(vad_evidence.begin(), vad_evidence.end(),
                           [](const auto& item) { return item.event == "vad_silence"; }),
          "小于端点窗口的自然停顿不得结束采集");
    Check(vad_input.EmitCapture(SpeechFrame(0, 0)).ok(), "自然停顿后的后续语音必须继续上送");
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    Check(vad_input.EmitCapture(SilenceFrame(0, 0)).ok() &&
              std::count_if(vad_evidence.begin(), vad_evidence.end(),
                            [](const auto& item) { return item.event == "vad_silence"; }) == 1,
          "超过端点窗口的连续静音必须只触发一次端点");
    Check(vad_session.Stop().ok(), "VAD 回归会话应可停止");
    auto mismatched_format = Frame(generation, 1);
    mismatched_format.format.sample_rate_hz = 8000;
    Check(session.SubmitAudio(std::move(mismatched_format)).code == ErrorCode::kInvalidArgument,
          "采样率与会话不一致的音频帧必须拒绝");
    auto mismatched_codec = Frame(generation, 1);
    mismatched_codec.format.codec = voicelife::voice::AudioCodec::kOpus;
    Check(session.SubmitAudio(std::move(mismatched_codec)).code == ErrorCode::kInvalidArgument,
          "编码与会话不一致的音频帧必须拒绝");
    Check(session.SubmitAudio(Frame(generation, 3)).code == ErrorCode::kConflict, "跳号音频帧必须拒绝");
    Check(session.SubmitAudio(Frame(generation - 1, 1)).code == ErrorCode::kInvalidArgument,
          "旧 generation 音频帧必须拒绝");
    provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kAsrText,
                                               .generation = generation,
                                               .text = "今天天气",
                                               .aborted = false});
    provider.Emit(voicelife::voice::VoiceEvent{
        .kind = voicelife::voice::VoiceEventKind::kTtsStarted, .generation = generation, .text = {}, .aborted = false});
    Check(input.stops == 2 && session.state() == voicelife::voice::VoiceSessionState::kSpeaking,
          "采集中收到 TTS start 必须先停止本地采集，再进入播放状态");
    Check(provider.EmitAudio(Frame(generation, 1)).ok() && output.pushes == 2,
          "停止采集后的 TTS 二进制帧必须进入输出端口");
    provider.Emit(voicelife::voice::VoiceEvent{
        .kind = voicelife::voice::VoiceEventKind::kTtsStopped, .generation = generation, .text = {}, .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady, "正常 TTS stop 后会话必须回到 ready");
    Check(input.EmitCapture(Frame(0, 0)).code == ErrorCode::kUnavailable, "停止采集后迟到的输入帧必须拒绝");
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
    Check(provider.EmitAudio(Frame(generation, 1)).code == ErrorCode::kUnavailable && output.pushes == 2,
          "打断后迟到的旧 generation 音频不得重新进入播放队列");
    provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                               .generation = session.generation(),
                                               .text = {},
                                               .aborted = false});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady,
          "打断后即使迟到 TTS 被归入新 generation 也不得复活旧播报");
    provider.Emit(voicelife::voice::VoiceEvent{});
    Check(session.state() == voicelife::voice::VoiceSessionState::kReady,
          "缺少 generation 的迟到 Provider 事件不能改变新会话状态");
    Check(session.Stop().ok() && session.state() == voicelife::voice::VoiceSessionState::kStopped,
          "停止应关闭 Provider 和音频端口");
    Check(close_order == std::vector<char>{'i', 'o'}, "共享双工设备必须先停止输入，再关闭输出 codec");
    Check(input.EmitCapture(Frame(0, 0)).code == ErrorCode::kUnavailable,
          "停止会话应清理输入回调，避免资源关闭后的迟到帧");
    Check(evidence_count >= 4, "会话生命周期应产出可关联的证据事件");

    FakeInput wake_input;
    FakeOutput wake_output;
    FakeProvider wake_provider;
    std::vector<voicelife::voice::VoiceEvidence> wake_evidence;
    voicelife::voice::VoiceSession wake_session(wake_input, wake_output, wake_provider,
                                                [&wake_evidence](const auto& item) { wake_evidence.push_back(item); });
    auto wake_config = Config();
    wake_config.mode = voicelife::voice::VoiceMode::kAuto;
    Check(wake_session.Start(wake_config).ok() && wake_session.NotifyLocalWakeWord("你好牛牛").ok(),
          "普通唤醒应先提交 detect");
    Check(wake_provider.calls == std::vector<std::string>{"listen.detect"} && wake_provider.starts == 0 &&
              wake_input.starts == 0,
          "detect 后尚未进入协议监听时不得打开物理输入");
    Check(wake_session.BeginProviderCapture().ok() &&
              wake_provider.calls == std::vector<std::string>{"listen.detect", "listen.start"} &&
              wake_provider.starts == 1 && wake_input.starts == 0,
          "普通唤醒必须按 detect -> listen.start 顺序发送且暂不打开物理输入");
    const uint64_t wake_generation = wake_session.generation();
    Check(wake_input.EmitCapture(Frame(wake_generation, 0)).code == ErrorCode::kUnavailable,
          "问候 TTS 阶段的物理 PCM 不得进入 Provider");
    wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kAsrText,
                                                    .generation = wake_generation,
                                                    .text = "你好牛牛",
                                                    .aborted = false});
    Check(!wake_evidence.empty() && wake_evidence.back().event == "wake_echo_suppressed",
          "服务端回传唤醒词必须只抑制一次，不得武装错误回复");
    wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                                    .generation = wake_generation,
                                                    .text = {},
                                                    .aborted = false});
    wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                                    .generation = wake_generation,
                                                    .text = {},
                                                    .aborted = false});
    Check(wake_session.BeginCapture().ok() && wake_provider.starts == 1 && wake_input.starts == 1,
          "问候结束后应只开启物理采集，不重复发送 listen.start");
    Check(wake_session.EndCapture().ok(), "普通唤醒后的真实采集应可正常结束");

    FakeInput acknowledged_wake_input;
    FakeOutput acknowledged_wake_output;
    FakeProvider acknowledged_wake_provider;
    std::vector<voicelife::voice::VoiceEvidence> acknowledged_wake_evidence;
    voicelife::voice::VoiceSession acknowledged_wake_session(
        acknowledged_wake_input, acknowledged_wake_output, acknowledged_wake_provider,
        [&acknowledged_wake_evidence](const auto& item) { acknowledged_wake_evidence.push_back(item); });
    Check(acknowledged_wake_session.Start(Config()).ok(), "确认唤醒回归会话应启动");
    Check(acknowledged_wake_session.NotifyLocalWakeWord("你好牛牛", "收到！").ok() &&
              acknowledged_wake_provider.wake_notifications == 1 &&
              acknowledged_wake_provider.last_wake_word == "你好牛牛" &&
              acknowledged_wake_provider.last_wake_response == "收到！" && acknowledged_wake_provider.starts == 0,
          "确认播报必须先请求服务端 TTS，未完成前不得打开采集");
    const uint64_t acknowledgement_generation = acknowledged_wake_session.generation();
    acknowledged_wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                                                 .generation = acknowledgement_generation,
                                                                 .text = {},
                                                                 .aborted = false});
    Check(acknowledged_wake_session.state() == voicelife::voice::VoiceSessionState::kSpeaking,
          "确认 TTS 开始后会话必须进入 speaking");
    acknowledged_wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                                                 .generation = acknowledgement_generation,
                                                                 .text = {},
                                                                 .aborted = false});
    Check(acknowledged_wake_session.state() == voicelife::voice::VoiceSessionState::kReady,
          "确认 TTS 结束后会话必须回 ready，等待状态机显式打开采集");
    Check(acknowledged_wake_session.BeginCapture().ok() && acknowledged_wake_provider.starts == 1 &&
              acknowledged_wake_input.starts == 1,
          "确认播报完成后必须能够事务式开始真实采集");
    const uint64_t wake_capture_generation = acknowledged_wake_session.generation();
    const int wake_capture_flushes = acknowledged_wake_output.flushes;
    acknowledged_wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                                                 .generation = wake_capture_generation,
                                                                 .text = {},
                                                                 .aborted = true});
    Check(acknowledged_wake_session.state() == voicelife::voice::VoiceSessionState::kCapturing &&
              acknowledged_wake_session.generation() == wake_capture_generation &&
              acknowledged_wake_output.flushes == wake_capture_flushes &&
              acknowledged_wake_evidence.back().event == "stale_event_dropped",
          "采集中的迟到 tts.stop/abort 不能推进代次、清空输出或中止真实指令");
    acknowledged_wake_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kAsrText,
                                                                 .generation = wake_capture_generation,
                                                                 .text = "现在几点",
                                                                 .aborted = false});
    Check(std::any_of(acknowledged_wake_evidence.begin(), acknowledged_wake_evidence.end(),
                      [](const auto& item) { return item.event == "stt_text_received" && item.detail == "现在几点"; }),
          "确认完成后的真实指令必须照常上报");
    Check(acknowledged_wake_session.EndCapture().ok(), "确认后的采集回合必须可以正常结束");

    FakeInput timed_out_ack_input;
    FakeOutput timed_out_ack_output;
    FakeProvider timed_out_ack_provider;
    std::vector<voicelife::voice::VoiceEvidence> timed_out_ack_evidence;
    voicelife::voice::VoiceSession timed_out_ack_session(
        timed_out_ack_input, timed_out_ack_output, timed_out_ack_provider,
        [&timed_out_ack_evidence](const auto& item) { timed_out_ack_evidence.push_back(item); });
    Check(timed_out_ack_session.Start(Config()).ok() &&
              timed_out_ack_session.NotifyLocalWakeWord("你好牛牛", "收到！").ok(),
          "确认首音频超时用例应能提交远端确认");
    const uint64_t timed_out_ack_generation = timed_out_ack_session.generation();
    timed_out_ack_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                                             .generation = timed_out_ack_generation,
                                                             .text = {},
                                                             .aborted = false});
    Check(timed_out_ack_session.state() == voicelife::voice::VoiceSessionState::kSpeaking &&
              timed_out_ack_session.Interrupt().ok() &&
              timed_out_ack_session.generation() == timed_out_ack_generation + 1 && timed_out_ack_provider.aborts == 1,
          "确认流没有首段音频时必须可中止，并使旧代次失效");
    Check(timed_out_ack_session.BeginCapture().ok() &&
              timed_out_ack_session.state() == voicelife::voice::VoiceSessionState::kCapturing,
          "跳过迟到确认后必须立即可开始真实采集");
    const uint64_t timed_out_capture_generation = timed_out_ack_session.generation();
    timed_out_ack_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                                             .generation = timed_out_ack_generation,
                                                             .text = {},
                                                             .aborted = true});
    Check(timed_out_ack_session.state() == voicelife::voice::VoiceSessionState::kCapturing &&
              timed_out_ack_session.generation() == timed_out_capture_generation &&
              timed_out_ack_evidence.back().event == "stale_event_dropped",
          "超时确认的迟到结束事件不得中止后续真实采集");

    FakeInput finalizing_input;
    FakeOutput finalizing_output;
    FakeProvider finalizing_provider;
    voicelife::voice::VoiceSession finalizing_session(finalizing_input, finalizing_output, finalizing_provider);
    Check(finalizing_session.Start(Config()).ok() && finalizing_session.BeginCapture().ok() &&
              finalizing_session.EndCapture().ok(),
          "最终识别取消用例应先停止采集并保留最终 STT 等待");
    const uint64_t finalizing_generation = finalizing_session.generation();
    Check(finalizing_session.Interrupt().ok() && finalizing_session.generation() == finalizing_generation + 1 &&
              finalizing_provider.aborts == 1 &&
              finalizing_provider.generation_at_abort == finalizing_session.generation(),
          "等待最终 STT 时打断必须推进 generation 并中止旧服务端回合");

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
    const uint64_t gen_before_stop_fail = stop_capture_failure.generation();
    Check(stop_capture_failure.EndCapture().ok() &&
              stop_capture_failure.state() == voicelife::voice::VoiceSessionState::kReady &&
              stop_capture_failure.generation() == gen_before_stop_fail + 1 &&
              stop_capture_failure_provider.generation_ == stop_capture_failure.generation(),
          "本地已停止而远端停止失败时回 ready 并使旧代次失效，不得卡死在 capturing");

    FakeInput duplicate_end_input;
    FakeOutput duplicate_end_output;
    FakeProvider duplicate_end_provider;
    voicelife::voice::VoiceSession duplicate_end(duplicate_end_input, duplicate_end_output, duplicate_end_provider);
    Check(duplicate_end.Start(Config()).ok() && duplicate_end.BeginCapture().ok() && duplicate_end.EndCapture().ok() &&
              duplicate_end.EndCapture().ok() && duplicate_end_input.stops == 1 && duplicate_end_provider.stops == 1,
          "重复结束采集必须幂等成功且只发送一次本地/远端 stop");

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
    // 模拟真实回合：进入聆听并收到 STT 后，TTS start 再接收协商格式下行音频。
    Check(negotiated_session.BeginCapture().ok(), "协商会话应可进入采集");
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kAsrText,
                                                          .generation = negotiated_session.generation(),
                                                          .text = "你好",
                                                          .aborted = false});
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStarted,
                                                          .generation = negotiated_session.generation(),
                                                          .text = {},
                                                          .aborted = false});
    Check(negotiated_session.state() == voicelife::voice::VoiceSessionState::kSpeaking,
          "协商会话进入聆听后收到 TTS start 应进入播报");
    Check(negotiated_provider.EmitAudio(std::move(negotiated_playback)).ok() && negotiated_output.pushes == 1,
          "协商后的 24 kHz 下行音频应进入输出端口");
    negotiated_session.EndCapture();
    negotiated_provider.Emit(voicelife::voice::VoiceEvent{.kind = voicelife::voice::VoiceEventKind::kTtsStopped,
                                                          .generation = negotiated_session.generation(),
                                                          .text = {},
                                                          .aborted = false});
    Check(negotiated_session.state() == voicelife::voice::VoiceSessionState::kReady, "协商会话结束播报后应回到 ready");
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
