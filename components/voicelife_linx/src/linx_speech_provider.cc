#include "voicelife/linx/linx_speech_provider.h"

#include <algorithm>
#include <chrono>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#endif

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

#ifdef ESP_PLATFORM
const char* MessageKindName(LinxMessageKind kind) {
    switch (kind) {
        case LinxMessageKind::kHello:
            return "hello";
        case LinxMessageKind::kStt:
            return "stt";
        case LinxMessageKind::kTts:
            return "tts";
        case LinxMessageKind::kMcp:
            return "mcp";
        case LinxMessageKind::kError:
            return "error";
        case LinxMessageKind::kGoodbye:
            return "goodbye";
        case LinxMessageKind::kLlm:
            return "llm";
    }
    return "unknown";
}

const char* TtsStateName(const std::optional<LinxTtsState>& state) {
    if (!state.has_value()) return "-";
    switch (*state) {
        case LinxTtsState::kStart:
            return "start";
        case LinxTtsState::kSentenceStart:
            return "sentence_start";
        case LinxTtsState::kStop:
            return "stop";
    }
    return "unknown";
}
#endif

}  // namespace

LinxSpeechProviderAdapter::LinxSpeechProviderAdapter(LinxTransportPort& transport, LinxProtocolCodecPort& codec,
                                                     LinxConnectionConfig connection,
                                                     voice::CapabilityProfile capabilities,
                                                     LinxMcpMessageHandler mcp_handler,
                                                     std::unique_ptr<voice::CodecStrategy> codec_strategy)
    : transport_(transport),
      codec_(codec),
      connection_(std::move(connection)),
      capabilities_(std::move(capabilities)),
      mcp_handler_(std::move(mcp_handler)),
      codec_strategy_(std::move(codec_strategy)) {}

LinxSpeechProviderAdapter::~LinxSpeechProviderAdapter() {
    (void)Disconnect();
    StopMcpWorker();
}

voice::CapabilityProfile LinxSpeechProviderAdapter::DefaultCapabilities() {
    return {.provider_id = "xrobot-websocket",
            .capabilities = {"streaming-asr", "tts", "cancel-generation", "pcm", "opus"}};
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
    if (const Status codec_status = ConfigureCodecStrategy(); !codec_status.ok()) {
        return codec_status;
    }
    explicit_disconnect_.store(false);
    transport_connected_.store(false);
    connected_.store(false);
    // Prime the transport gate before it can accept the first connection
    // callback. Otherwise the first session's PCM generation would differ
    // from the transport default and be silently dropped.
    SetGeneration(config.generation);
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
    StartMcpWorker();
    LinxTransportSink transport_sink;
    transport_sink.on_connected = [this]() { OnTransportConnected(); };
    transport_sink.on_disconnected = [this]() { OnTransportDisconnected(); };
    transport_sink.on_text = [this](std::string_view message) { OnText(message); };
    transport_sink.on_binary = [this](std::vector<uint8_t> payload) { OnBinary(std::move(payload)); };
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
        StopMcpWorker();
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
        StopMcpWorker();
        std::lock_guard<std::mutex> lock(callback_mutex_);
        event_sink_ = {};
        return Status::Error(ErrorCode::kUnavailable, "Linx hello 等待超时");
    }
    status = hello_status_;
    hello_lock.unlock();
    if (!status.ok()) {
        transport_.Close();
        StopMcpWorker();
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

Status LinxSpeechProviderAdapter::SendAudio(voice::AudioFrame frame) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    if (frame.generation != generation_.load()) {
        return Status::Error(ErrorCode::kConflict, "Linx 音频帧属于旧连接代次");
    }
    if (!SameFormat(frame.format, config_.audio)) {
        return Status::Error(ErrorCode::kInvalidArgument, "Linx 上行帧不是本地协商 PCM 格式");
    }
    const voice::AudioFormat wire = WireAudioFormat();
    if (wire.codec != frame.format.codec) {
        if (codec_strategy_ == nullptr || codec_strategy_->codec() != wire.codec) {
            return Status::Error(ErrorCode::kUnavailable, "Linx 上行缺少已配置的音频转码策略");
        }
        auto encoded = codec_strategy_->Encode(frame);
        if (!encoded.ok() || !encoded.value.has_value()) return encoded.status;
        frame = std::move(*encoded.value);
        if (!SameFormat(frame.format, wire)) {
            return Status::Error(ErrorCode::kInvalidArgument, "Linx 编码器返回的线上格式与 hello 不一致");
        }
    }
    return transport_.SendAudio(std::move(frame));
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
    // Linx formally defines text_response on listen.detect as the device's
    // requested server-side TTS. Keep the protocol field in this adapter;
    // Runtime and VoiceSession only express semantic system speech.
    return Send(codec_.EncodeListenDetect(ActiveSessionConfig(), "system_prompt", text));
}

Status LinxSpeechProviderAdapter::NotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response) {
    if (!connected_.load()) {
        return Status::Error(ErrorCode::kUnavailable, "Linx Provider 尚未连接");
    }
    if (wake_word.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "本地唤醒词为空");
    }
    return Send(codec_.EncodeListenDetect(ActiveSessionConfig(), wake_word, text_response));
}

Status LinxSpeechProviderAdapter::Disconnect() {
    explicit_disconnect_.store(true);
    const Status status = transport_.Close();
    StopMcpWorker();
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
#ifdef ESP_PLATFORM
    const voice::AudioFormat wire = WireAudioFormat();
    ESP_LOGI("voicelife_linx",
             "LINX_HELLO_REQUEST local_format=%d wire_format=%d sample_rate=%u channels=%u bits=%u frame_ms=%u "
             "play_buffer_ms=%u",
             static_cast<int>(config_.audio.codec), static_cast<int>(wire.codec),
             static_cast<unsigned>(wire.sample_rate_hz), static_cast<unsigned>(wire.channels),
             static_cast<unsigned>(wire.bits_per_sample), static_cast<unsigned>(wire.frame_duration_ms),
             static_cast<unsigned>(connection_.playback_buffer_duration_ms));
#endif
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

voice::AudioFormat LinxSpeechProviderAdapter::WireAudioFormat() const {
    return connection_.preferred_audio.value_or(config_.audio);
}

Status LinxSpeechProviderAdapter::ConfigureCodecStrategy() {
    const voice::AudioFormat wire = WireAudioFormat();
    if (wire.codec == config_.audio.codec) return Status::Ok();
    if (codec_strategy_ == nullptr || codec_strategy_->codec() != wire.codec) {
        return Status::Error(ErrorCode::kUnavailable, "Linx hello 请求的编码缺少转码策略");
    }
    return codec_strategy_->Configure(config_.audio, wire);
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
    // 脱敏诊断：仅记录控制消息的 type/state 字段，不输出 token、设备 ID 或完整消息。
    const std::string& message = *encoded.value;
    // 控制消息的脱敏 type/state 日志由 transport 层记录（见 EspWebSocketTransport::SendText）。
    return transport_.SendText(message);
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
#ifdef ESP_PLATFORM
    // Record the server's control sequence without exposing credentials. STT
    // and TTS text are intentionally visible on the authorized hardware log so
    // a reset can be correlated with the last protocol event.
    const std::string_view text = inbound.text;
    ESP_LOGI("voicelife_linx", "LINX_RX kind=%s tts_state=%s session_present=%d session_len=%u text=%.*s",
             MessageKindName(inbound.kind), TtsStateName(inbound.tts_state), inbound.session_id.has_value() ? 1 : 0,
             inbound.session_id.has_value() ? static_cast<unsigned>(inbound.session_id->size()) : 0U,
             static_cast<int>(std::min<std::size_t>(text.size(), 160U)), text.data());
#endif
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
#ifdef ESP_PLATFORM
                ESP_LOGI("voicelife_linx",
                         "LINX_HELLO_RESPONSE format=%d sample_rate=%u channels=%u bits=%u frame_ms=%u "
                         "session_present=%d session_len=%u",
                         static_cast<int>(negotiated.codec), static_cast<unsigned>(negotiated.sample_rate_hz),
                         static_cast<unsigned>(negotiated.channels), static_cast<unsigned>(negotiated.bits_per_sample),
                         static_cast<unsigned>(negotiated.frame_duration_ms), inbound.session_id.has_value() ? 1 : 0,
                         inbound.session_id.has_value() ? static_cast<unsigned>(inbound.session_id->size()) : 0U);
#endif
                const voice::AudioFormat wire = WireAudioFormat();
                const voice::AudioFormat server_wire = {.codec = negotiated.codec,
                                                        .sample_rate_hz = negotiated.sample_rate_hz,
                                                        .channels = negotiated.channels,
                                                        .bits_per_sample = negotiated.bits_per_sample,
                                                        .frame_duration_ms = negotiated.frame_duration_ms};
                const bool is_transcoded_wire = wire.codec != config_.audio.codec;
                if ((is_transcoded_wire && !SameFormat(server_wire, wire)) ||
                    (!is_transcoded_wire && server_wire.codec != wire.codec)) {
                    Emit(Event(voice::VoiceEventKind::kError, "Linx hello 返回了当前固件未配置的线上音频格式"));
                    {
                        std::lock_guard<std::mutex> lock(hello_mutex_);
                        hello_received_ = true;
                        hello_status_ =
                            Status::Error(ErrorCode::kInvalidArgument, "Linx hello 返回了当前固件未配置的线上音频格式");
                    }
                    hello_cv_.notify_all();
                    return;
                }
                // Local AudioInput/Output remain PCM. If the wire codec is
                // also PCM, preserve the server's downlink format as before.
                if (wire.codec == config_.audio.codec) formats.playback = server_wire;
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
            const std::string session_id = inbound.session_id.value_or(ActiveSessionConfig().session_id);
            bool rejected = false;
            {
                std::lock_guard<std::mutex> lock(mcp_mutex_);
                if (mcp_stop_ || mcp_queue_.size() >= kMcpQueueCapacity) {
                    rejected = true;
                } else {
                    mcp_queue_.push_back(McpRequest{
                        .payload = inbound.text, .session_id = session_id, .generation = generation_.load()});
                }
            }
            if (rejected) {
                Emit(Event(voice::VoiceEventKind::kError, "Linx MCP 请求队列已满"));
                return;
            }
            mcp_cv_.notify_one();
            return;
        }
        case LinxMessageKind::kGoodbye:
            // 服务端结束会话的告别消息：不是故障，保持当前状态等待断开事件。
            return;
        case LinxMessageKind::kLlm:
            // 服务端表情/情感 UI 消息：本板仅文本 OLED，无表情渲染，直接忽略。
            return;
        case LinxMessageKind::kError:
            Emit(Event(voice::VoiceEventKind::kError, inbound.text));
            return;
    }
}

void LinxSpeechProviderAdapter::StartMcpWorker() {
    if (!mcp_handler_) return;
    std::lock_guard<std::mutex> lock(mcp_mutex_);
    if (mcp_worker_.joinable()) return;
    mcp_stop_ = false;
#ifdef ESP_PLATFORM
    // MCP handlers wait on the Runtime worker and may carry a large JSON-RPC
    // response through std::function/condition_variable frames. The ESP-IDF
    // pthread default is only 3072 bytes and is allocated from internal RAM;
    // that is insufficient for the first initialize/tools/list exchange and
    // canaries report it later as a "task pthread" overflow. Give this one
    // worker a bounded PSRAM stack without changing other pthread users.
    esp_pthread_cfg_t pthread_config = esp_pthread_get_default_config();
    pthread_config.stack_size = 16 * 1024;
    pthread_config.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    pthread_config.thread_name = "voicelife_linx_mcp";
    pthread_config.inherit_cfg = false;
    if (esp_pthread_set_cfg(&pthread_config) != ESP_OK) {
        ESP_LOGW("voicelife_linx", "LINX_MCP_PTHREAD_CONFIG_FAILED=1");
    } else {
        ESP_LOGI("voicelife_linx", "LINX_MCP_PTHREAD_CONFIG stack_bytes=%u caps=spiram",
                 static_cast<unsigned>(pthread_config.stack_size));
    }
#endif
    mcp_worker_ = std::thread([this]() { McpWorkerLoop(); });
}

void LinxSpeechProviderAdapter::StopMcpWorker() {
    {
        std::lock_guard<std::mutex> lock(mcp_mutex_);
        mcp_stop_ = true;
        mcp_queue_.clear();
    }
    mcp_cv_.notify_all();
    if (mcp_worker_.joinable()) mcp_worker_.join();
}

void LinxSpeechProviderAdapter::McpWorkerLoop() {
    while (true) {
        McpRequest request;
        {
            std::unique_lock<std::mutex> lock(mcp_mutex_);
            mcp_cv_.wait(lock, [this]() { return mcp_stop_ || !mcp_queue_.empty(); });
            if (mcp_stop_ && mcp_queue_.empty()) return;
            request = std::move(mcp_queue_.front());
            mcp_queue_.pop_front();
        }
        if (request.generation != generation_.load() || !connected_.load()) continue;
        const auto response = mcp_handler_(request.payload, request.session_id);
        if (request.generation != generation_.load() || !connected_.load()) continue;
        if (!response.ok() || !response.value.has_value()) {
            Emit(Event(voice::VoiceEventKind::kError, response.status.message));
            continue;
        }
        if (response.value->empty()) continue;
        const Status status = transport_.SendText(*response.value);
        if (!status.ok()) Emit(Event(voice::VoiceEventKind::kError, status.message));
    }
}

void LinxSpeechProviderAdapter::OnBinary(std::vector<uint8_t> payload) {
    if (!connected_.load()) {
        Emit(Event(voice::VoiceEventKind::kError, "Linx hello 未完成，拒绝下行音频"));
        return;
    }
    if (payload.empty()) {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频帧为空"));
        return;
    }
    if (payload.size() > voice::AudioFrame::kMaxPayloadBytes) {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频帧超过单帧内存上限"));
        return;
    }
    voice::AudioFrame frame;
    frame.generation = generation_.load();
    frame.sequence = output_sequence_.fetch_add(1);
    const voice::AudioFormat wire = WireAudioFormat();
    if (wire.codec == config_.audio.codec) {
        std::lock_guard<std::mutex> lock(hello_mutex_);
        frame.format = audio_formats_.playback;
    } else {
        frame.format = wire;
    }
    frame.payload = std::move(payload);
    if (frame.format.codec != config_.audio.codec) {
        if (codec_strategy_ == nullptr || codec_strategy_->codec() != frame.format.codec) {
            Emit(Event(voice::VoiceEventKind::kError, "Linx 下行缺少已配置的音频解码策略"));
            return;
        }
        auto decoded = codec_strategy_->Decode(frame);
        if (!decoded.ok() || !decoded.value.has_value()) {
            // A bounded codec pool can reject a burst after all playable PCM
            // slots are occupied. Treat that as a per-frame drop, like an
            // output-queue conflict, rather than a provider lifecycle fault.
            if (decoded.status.code == ErrorCode::kConflict) return;
            Emit(Event(voice::VoiceEventKind::kError, decoded.status.message));
            return;
        }
        frame = std::move(*decoded.value);
        if (!SameFormat(frame.format, config_.audio)) {
            Emit(Event(voice::VoiceEventKind::kError, "Linx 解码器返回的本地 PCM 格式不匹配"));
            return;
        }
    }
    voice::AudioFrameSink sink;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        sink = audio_sink_;
    }
    if (sink) {
        const Status status = sink(std::move(frame));
        if (!status.ok()) {
            // The board's output queue is deliberately bounded. A burst can
            // reject one frame while playback remains healthy; metrics record
            // that loss, so do not turn it into a provider lifecycle failure.
            // kConflict / kUnavailable are expected state guards (stale
            // generation, residual TTS outside a speaking turn); drop them
            // silently instead of surfacing a false provider error.
            if (status.code == ErrorCode::kConflict || status.code == ErrorCode::kUnavailable) return;
            Emit(Event(voice::VoiceEventKind::kError, status.message));
        }
    } else {
        Emit(Event(voice::VoiceEventKind::kError, "Linx 下行音频没有绑定输出端口"));
    }
}

}  // namespace voicelife::linx
