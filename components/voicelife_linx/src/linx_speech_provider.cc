#include "voicelife/linx/linx_speech_provider.h"

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

}  // namespace

LinxSpeechProviderAdapter::LinxSpeechProviderAdapter(
    LinxTransportPort& transport, LinxProtocolCodecPort& codec, LinxConnectionConfig connection,
    voice::CapabilityProfile capabilities)
    : transport_(transport),
      codec_(codec),
      connection_(std::move(connection)),
      capabilities_(std::move(capabilities)) {}

voice::CapabilityProfile LinxSpeechProviderAdapter::DefaultCapabilities() {
    return {.provider_id = "xrobot-websocket",
            .capabilities = {"streaming-asr", "tts", "cancel-generation", "pcm", "opus"}};
}

void LinxSpeechProviderAdapter::SetAudioSink(voice::AudioFrameSink sink) {
    audio_sink_ = std::move(sink);
}

void LinxSpeechProviderAdapter::SetGeneration(uint64_t generation) {
    if (connected_ && generation != 0) {
        generation_ = generation;
        output_sequence_ = 0;
    }
}

Status LinxSpeechProviderAdapter::Connect(const voice::VoiceSessionConfig& config,
                                          voice::VoiceEventSink sink) {
    if (!connection_.valid() || config.provider_id != capabilities_.provider_id || config.generation == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "Linx Provider 连接配置无效");
    }
    if (connected_) {
        return Status::Error(ErrorCode::kConflict, "Linx Provider 已连接");
    }
    config_ = config;
    generation_ = config.generation;
    output_sequence_ = 0;
    event_sink_ = std::move(sink);
    LinxTransportSink transport_sink;
    transport_sink.on_text = [this](std::string_view message) { OnText(message); };
    transport_sink.on_binary = [this](const std::vector<uint8_t>& payload) { OnBinary(payload); };
    Status status = transport_.Connect(connection_, std::move(transport_sink));
    if (!status.ok()) {
        event_sink_ = {};
        return status;
    }
    status = Send(codec_.EncodeHello(config_, connection_));
    if (!status.ok()) {
        transport_.Close();
        event_sink_ = {};
        return status;
    }
    connected_ = true;
    return Status::Ok();
}

Status LinxSpeechProviderAdapter::StartCapture(voice::VoiceMode) {
    if (!connected_) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeListenStart(config_));
}

Status LinxSpeechProviderAdapter::StopCapture() {
    if (!connected_) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeListenStop(config_));
}

Status LinxSpeechProviderAdapter::SendAudio(const voice::AudioFrame& frame) {
    if (!connected_) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    if (frame.generation != generation_) {
        return Status::Error(ErrorCode::kConflict, "Linx 音频帧属于旧连接代次");
    }
    return transport_.SendAudio(frame);
}

Status LinxSpeechProviderAdapter::Abort(std::string_view reason) {
    if (!connected_) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeAbort(config_, reason));
}

Status LinxSpeechProviderAdapter::Speak(std::string_view text) {
    if (!connected_) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    return Send(codec_.EncodeListenDetect(config_, text));
}

Status LinxSpeechProviderAdapter::Disconnect() {
    const Status status = transport_.Close();
    connected_ = false;
    generation_ = 0;
    output_sequence_ = 0;
    event_sink_ = {};
    audio_sink_ = {};
    return status;
}

Status LinxSpeechProviderAdapter::Send(Result<std::string> encoded) {
    if (!encoded.ok() || !encoded.value.has_value()) {
        return encoded.status;
    }
    return transport_.SendText(*encoded.value);
}

void LinxSpeechProviderAdapter::Emit(voice::VoiceEvent event) {
    event.generation = generation_;
    if (event_sink_) {
        event_sink_(event);
    }
}

void LinxSpeechProviderAdapter::OnText(std::string_view message) {
    auto decoded = codec_.DecodeText(message);
    if (!decoded.ok() || !decoded.value.has_value()) {
        Emit(Event(voice::VoiceEventKind::kError, decoded.status.message));
        return;
    }
    const LinxInboundMessage& inbound = *decoded.value;
    switch (inbound.kind) {
        case LinxMessageKind::kHello:
            if (inbound.audio_params.has_value()) {
                const LinxAudioParams& negotiated = *inbound.audio_params;
                if (negotiated.codec != config_.audio.codec ||
                    negotiated.sample_rate_hz != config_.audio.sample_rate_hz ||
                    negotiated.channels != config_.audio.channels ||
                    negotiated.bits_per_sample != config_.audio.bits_per_sample ||
                    negotiated.frame_duration_ms != config_.audio.frame_duration_ms) {
                    Emit(Event(voice::VoiceEventKind::kError, "Linx hello 音频参数与会话请求不一致"));
                    return;
                }
            }
            Emit(Event(voice::VoiceEventKind::kConnected));
            return;
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
        case LinxMessageKind::kError:
            Emit(Event(voice::VoiceEventKind::kError, inbound.text));
            return;
    }
}

void LinxSpeechProviderAdapter::OnBinary(const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频帧为空"));
        return;
    }
    voice::AudioFrame frame;
    frame.generation = generation_;
    frame.sequence = output_sequence_++;
    frame.format = config_.audio;
    frame.payload = payload;
    if (audio_sink_) {
        const Status status = audio_sink_(std::move(frame));
        if (!status.ok()) {
            Emit(Event(voice::VoiceEventKind::kError, status.message));
        }
    } else {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频没有绑定输出端口"));
    }
}

}  // namespace voicelife::linx
