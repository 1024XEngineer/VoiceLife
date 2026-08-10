#include "voicelife/linx/linx_speech_provider.h"

#include <chrono>
#include <utility>

namespace voicelife::linx {
namespace {

voice::VoiceEvent Event(voice::VoiceEventKind kind, std::string_view text = {}, bool aborted = false) {
    voice::VoiceEvent event;
    event.kind = kind;
    event.text = text;
    event.aborted = aborted;
    return event;
}

bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right) {
    return left.codec == right.codec && left.sample_rate_hz == right.sample_rate_hz &&
           left.channels == right.channels && left.bits_per_sample == right.bits_per_sample &&
           left.frame_duration_ms == right.frame_duration_ms;
}

bool SameAudioFormats(const voice::VoiceAudioFormats& left, const voice::VoiceAudioFormats& right) {
    return SameFormat(left.capture, right.capture) && SameFormat(left.playback, right.playback);
}

}  // namespace

LinxSpeechProviderAdapter::LinxSpeechProviderAdapter(LinxTransportPort& transport, LinxProtocolCodecPort& codec,
                                                     LinxConnectionConfig connection,
                                                     voice::CapabilityProfile capabilities,
                                                     LinxMcpMessageHandler mcp_handler)
    : transport_(transport),
      codec_(codec),
      connection_(std::move(connection)),
      capabilities_(std::move(capabilities)),
      mcp_handler_(std::move(mcp_handler)) {}

voice::CapabilityProfile LinxSpeechProviderAdapter::DefaultCapabilities() {
    return {.provider_id = "xrobot-websocket", .capabilities = {"streaming-asr", "tts", "cancel-generation", "pcm"}};
}

void LinxSpeechProviderAdapter::SetAudioSink(voice::AudioFrameSink sink) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    audio_sink_ = std::move(sink);
}

void LinxSpeechProviderAdapter::SetGeneration(uint64_t generation) {
    if (generation != 0) {
        generation_.store(generation);
        output_sequence_.store(0);
        transport_.SetGeneration(generation);
    }
}

Status LinxSpeechProviderAdapter::Connect(const voice::VoiceSessionConfig& config, voice::VoiceEventSink sink) {
    if (!connection_.valid() || config.provider_id != capabilities_.provider_id || config.generation == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "Linx Provider 连接配置无效");
    }
    if (connected_.load() || transport_connected_.load()) {
        return Status::Error(ErrorCode::kConflict, "Linx Provider 或底层 Transport 已连接");
    }
    config_ = config;
    explicit_disconnect_.store(false);
    transport_connected_.store(false);
    connected_.store(false);
    generation_.store(config.generation);
    output_sequence_.store(0);
    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        hello_received_ = false;
        audio_formats_ready_ = false;
        has_negotiated_formats_ = false;
        remote_session_id_.reset();
        audio_formats_ = {.capture = config.audio, .playback = config.audio};
        last_audio_formats_ = audio_formats_;
        hello_status_ = Status::Ok();
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        event_sink_ = std::move(sink);
    }
    LinxTransportSink transport_sink;
    transport_sink.on_connected = [this]() { OnTransportConnected(); };
    transport_sink.on_disconnected = [this]() { OnTransportDisconnected(); };
    transport_sink.on_text = [this](std::string_view message) { OnText(message); };
    transport_sink.on_binary = [this](const std::vector<uint8_t>& payload) { OnBinary(payload); };
    transport_sink.on_error = [this](Status status) {
        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(hello_mutex_);
            if (!hello_received_) {
                hello_status_ = status;
                hello_received_ = true;
            }
        }
        hello_cv_.notify_all();
        Emit(Event(voice::VoiceEventKind::kError, status.message));
    };
    Status status = transport_.Connect(connection_, std::move(transport_sink));
    if (!status.ok()) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        event_sink_ = {};
        return status;
    }
    std::unique_lock<std::mutex> hello_lock(hello_mutex_);
    const bool received = hello_cv_.wait_for(hello_lock, std::chrono::milliseconds(config_.hello_timeout_ms),
                                             [this]() { return hello_received_; });
    if (!received) {
        hello_lock.unlock();
        transport_.Close();
        std::lock_guard<std::mutex> lock(callback_mutex_);
        event_sink_ = {};
        return Status::Error(ErrorCode::kUnavailable, "Linx hello 等待超时");
    }
    status = hello_status_;
    hello_lock.unlock();
    if (!status.ok()) {
        transport_.Close();
        std::lock_guard<std::mutex> lock(callback_mutex_);
        event_sink_ = {};
        return status;
    }
    return Status::Ok();
}

Result<voice::VoiceAudioFormats> LinxSpeechProviderAdapter::audio_formats() const {
    std::lock_guard<std::mutex> lock(hello_mutex_);
    if (!connected_.load() || !audio_formats_ready_) {
        return Result<voice::VoiceAudioFormats>::Failure(ErrorCode::kUnavailable, "Linx hello 尚未完成音频格式协商");
    }
    return Result<voice::VoiceAudioFormats>::Success(audio_formats_);
}

Status LinxSpeechProviderAdapter::StartCapture(voice::VoiceMode) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeListenStart(ActiveSessionConfig()));
}

Status LinxSpeechProviderAdapter::StopCapture() {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeListenStop(ActiveSessionConfig()));
}

Status LinxSpeechProviderAdapter::SendAudio(const voice::AudioFrame& frame) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    if (frame.generation != generation_.load()) {
        return Status::Error(ErrorCode::kConflict, "Linx 音频帧属于旧连接代次");
    }
    return transport_.SendAudio(frame);
}

Status LinxSpeechProviderAdapter::Abort(std::string_view reason) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeAbort(ActiveSessionConfig(), reason));
}

Status LinxSpeechProviderAdapter::Speak(std::string_view text) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeListenDetect(ActiveSessionConfig(), text));
}

Status LinxSpeechProviderAdapter::NotifyLocalWakeWord(std::string_view wake_word) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    if (wake_word.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "本地唤醒词为空");
    }
    return Send(codec_.EncodeListenDetect(ActiveSessionConfig(), wake_word));
}

Status LinxSpeechProviderAdapter::Disconnect() {
    explicit_disconnect_.store(true);
    const Status status = transport_.Close();
    hello_cv_.notify_all();
    connected_.store(false);
    transport_connected_.store(false);
    generation_.store(0);
    output_sequence_.store(0);
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        event_sink_ = {};
        audio_sink_ = {};
    }
    return status;
}

void LinxSpeechProviderAdapter::OnTransportConnected() {
    bool expected = false;
    if (!transport_connected_.compare_exchange_strong(expected, true)) {
        return;
    }
    connected_.store(false);
    output_sequence_.store(0);
    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        hello_received_ = false;
        audio_formats_ready_ = false;
        remote_session_id_.reset();
        hello_status_ = Status::Ok();
    }
    const Status status = Send(codec_.EncodeHello(config_, connection_));
    if (!status.ok()) {
        {
            std::lock_guard<std::mutex> lock(hello_mutex_);
            hello_received_ = true;
            hello_status_ = status;
        }
        hello_cv_.notify_all();
        Emit(Event(voice::VoiceEventKind::kError, status.message));
    }
}

voice::VoiceSessionConfig LinxSpeechProviderAdapter::ActiveSessionConfig() const {
    std::lock_guard<std::mutex> lock(hello_mutex_);
    auto config = config_;
    if (remote_session_id_.has_value()) {
        config.session_id = *remote_session_id_;
    }
    return config;
}

void LinxSpeechProviderAdapter::OnTransportDisconnected() {
    if (!transport_connected_.exchange(false)) {
        return;
    }
    connected_.store(false);
    output_sequence_.store(0);
    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        if (!hello_received_) {
            hello_received_ = true;
            hello_status_ = Status::Error(ErrorCode::kUnavailable, "Linx Transport 在 hello 完成前断开");
        }
    }
    hello_cv_.notify_all();
    if (!explicit_disconnect_.load()) {
        Emit(Event(voice::VoiceEventKind::kDisconnected));
    }
}

Status LinxSpeechProviderAdapter::Send(Result<std::string> encoded) {
    if (!encoded.ok() || !encoded.value.has_value()) {
        return encoded.status;
    }
    return transport_.SendText(*encoded.value);
}

void LinxSpeechProviderAdapter::Emit(voice::VoiceEvent event) {
    event.generation = generation_.load();
    voice::VoiceEventSink sink;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        sink = event_sink_;
    }
    if (sink) {
        sink(event);
    }
}

void LinxSpeechProviderAdapter::OnText(std::string_view message) {
    auto decoded = codec_.DecodeText(message);
    if (!decoded.ok() || !decoded.value.has_value()) {
        Emit(Event(voice::VoiceEventKind::kError, decoded.status.message));
        return;
    }
    const LinxInboundMessage& inbound = *decoded.value;
    // Linx assigns session_id in its hello response. Only that first hello
    // can establish the remote ID; all later messages must match it.
    if (inbound.kind != LinxMessageKind::kHello) {
        bool session_mismatch = !connected_.load();
        {
            std::lock_guard<std::mutex> lock(hello_mutex_);
            session_mismatch = session_mismatch || (remote_session_id_.has_value() && inbound.session_id.has_value() &&
                                                    *inbound.session_id != *remote_session_id_);
        }
        if (session_mismatch) {
            Emit(Event(voice::VoiceEventKind::kError, "Linx 消息 session_id 不匹配或 hello 未完成"));
            return;
        }
    }
    switch (inbound.kind) {
        case LinxMessageKind::kHello: {
            if (!transport_connected_.load()) {
                return;
            }
            voice::VoiceAudioFormats formats{.capture = config_.audio, .playback = config_.audio};
            if (!inbound.audio_params.has_value()) {
                Emit(Event(voice::VoiceEventKind::kError, "Linx hello 缺少 audio_params，无法确认音频格式协商"));
                {
                    std::lock_guard<std::mutex> lock(hello_mutex_);
                    hello_received_ = true;
                    hello_status_ = Status::Error(ErrorCode::kInvalidArgument,
                                                  "Linx hello 缺少 audio_params，无法确认音频格式协商");
                }
                hello_cv_.notify_all();
                return;
            }
            {
                const LinxAudioParams& negotiated = *inbound.audio_params;
                if (negotiated.codec != config_.audio.codec) {
                    Emit(Event(voice::VoiceEventKind::kError, "Linx hello 改变音频编码，但当前未配置转码策略"));
                    {
                        std::lock_guard<std::mutex> lock(hello_mutex_);
                        hello_received_ = true;
                        hello_status_ =
                            Status::Error(ErrorCode::kInvalidArgument, "Linx hello 改变音频编码，但当前未配置转码策略");
                    }
                    hello_cv_.notify_all();
                    return;
                }
                formats.playback = {.codec = negotiated.codec,
                                    .sample_rate_hz = negotiated.sample_rate_hz,
                                    .channels = negotiated.channels,
                                    .bits_per_sample = negotiated.bits_per_sample,
                                    .frame_duration_ms = negotiated.frame_duration_ms};
            }
            bool format_changed = false;
            Status format_status = Status::Ok();
            {
                std::lock_guard<std::mutex> lock(hello_mutex_);
                if (hello_received_ && connected_.load()) {
                    return;
                }
                if (has_negotiated_formats_ && !SameAudioFormats(last_audio_formats_, formats)) {
                    format_changed = true;
                    format_status =
                        Status::Error(ErrorCode::kInvalidArgument,
                                      "Linx 重连 hello 改变已协商音频格式，当前未配置 AudioOutput 重配置策略");
                    hello_received_ = true;
                    audio_formats_ready_ = false;
                    hello_status_ = format_status;
                } else if (inbound.session_id.has_value() && inbound.session_id->empty()) {
                    format_changed = true;
                    format_status = Status::Error(ErrorCode::kInvalidArgument, "Linx hello session_id 不能为空");
                    hello_received_ = true;
                    audio_formats_ready_ = false;
                    hello_status_ = format_status;
                } else {
                    remote_session_id_ = inbound.session_id;
                    hello_received_ = true;
                    audio_formats_ = formats;
                    last_audio_formats_ = formats;
                    has_negotiated_formats_ = true;
                    audio_formats_ready_ = formats.valid();
                    hello_status_ = Status::Ok();
                }
            }
            if (format_changed) {
                connected_.store(false);
                hello_cv_.notify_all();
                Emit(Event(voice::VoiceEventKind::kError, format_status.message));
                return;
            }
            connected_.store(true);
            hello_cv_.notify_all();
            Emit(Event(voice::VoiceEventKind::kConnected));
            return;
        }
        case LinxMessageKind::kStt:
            Emit(Event(voice::VoiceEventKind::kAsrText, inbound.text));
            return;
        case LinxMessageKind::kTts:
            if (!inbound.tts_state.has_value()) {
                Emit(Event(voice::VoiceEventKind::kError, "Linx TTS 缺少状态"));
                return;
            }
            if (*inbound.tts_state == LinxTtsState::kStart) {
                Emit(Event(voice::VoiceEventKind::kTtsStarted));
            } else if (*inbound.tts_state == LinxTtsState::kSentenceStart) {
                Emit(Event(voice::VoiceEventKind::kTtsSentenceStarted, inbound.text));
            } else {
                Emit(Event(voice::VoiceEventKind::kTtsStopped, {}, inbound.aborted));
            }
            return;
        case LinxMessageKind::kMcp: {
            if (!mcp_handler_) {
                Emit(Event(voice::VoiceEventKind::kError, "Linx 收到 MCP 请求，但设备未配置 MCP handler"));
                return;
            }
            Emit(Event(voice::VoiceEventKind::kToolCall));
            const std::string session_id = inbound.session_id.value_or(ActiveSessionConfig().session_id);
            if (const auto response = mcp_handler_(inbound.text, session_id);
                response.ok() && response.value.has_value()) {
                const Status status = transport_.SendText(*response.value);
                if (!status.ok()) Emit(Event(voice::VoiceEventKind::kError, status.message));
            } else {
                Emit(Event(voice::VoiceEventKind::kError, response.status.message));
            }
            return;
        }
        case LinxMessageKind::kError:
            Emit(Event(voice::VoiceEventKind::kError, inbound.text));
            return;
    }
}

void LinxSpeechProviderAdapter::OnBinary(const std::vector<uint8_t>& payload) {
    if (!connected_.load()) {
        Emit(Event(voice::VoiceEventKind::kError, "Linx hello 未完成，拒绝下行音频"));
        return;
    }
    if (payload.empty()) {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频帧为空"));
        return;
    }
    voice::AudioFrame frame;
    frame.generation = generation_.load();
    frame.sequence = output_sequence_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        frame.format = audio_formats_.playback;
    }
    frame.payload = payload;
    voice::AudioFrameSink sink;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        sink = audio_sink_;
    }
    if (sink) {
        const Status status = sink(std::move(frame));
        if (!status.ok()) {
            Emit(Event(voice::VoiceEventKind::kError, status.message));
        }
    } else {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频没有绑定输出端口"));
    }
}

}  // namespace voicelife::linx
