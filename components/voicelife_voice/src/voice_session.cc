#include "voicelife/voice/voice_session.h"

#include <string>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace voicelife::voice {

VoiceSession::VoiceSession(AudioInputPort& input, AudioOutputPort& output, SpeechProviderAdapter& provider,
                           EvidenceSink evidence)
    : input_(input), output_(output), provider_(provider), evidence_(std::move(evidence)) {}

void VoiceSession::Emit(std::string_view event, std::string_view detail) {
    VoiceEvidence evidence;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        evidence = {.session_id = config_.session_id,
                    .generation = generation_,
                    .event = std::string(event),
                    .detail = std::string(detail)};
    }
    if (evidence_) {
        evidence_(evidence);
    }
}

VoiceSessionState VoiceSession::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint64_t VoiceSession::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

VoiceSessionConfig VoiceSession::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

AudioFormat VoiceSession::playback_format() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audio_formats_.playback;
}

bool VoiceSession::AcceptFrameLocked(const AudioFrame& frame) const {
    const AudioFormat& expected = audio_formats_.capture;
    const AudioFormat& actual = frame.format;
    return state_ == VoiceSessionState::kCapturing && frame.generation == generation_ && actual.valid() &&
           actual.codec == expected.codec && actual.sample_rate_hz == expected.sample_rate_hz &&
           actual.channels == expected.channels && actual.bits_per_sample == expected.bits_per_sample &&
           actual.frame_duration_ms == expected.frame_duration_ms && !frame.payload.empty() &&
           frame.payload.size() <= AudioFrame::kMaxPayloadBytes;
}

Status VoiceSession::Start(const VoiceSessionConfig& config) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (config.session_id.empty() || config.provider_id.empty() || !config.audio.valid() ||
        config.hello_timeout_ms == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "语音会话配置无效");
    }
    if (provider_.capabilities().provider_id != config.provider_id) {
        return Status::Error(ErrorCode::kInvalidArgument, "Provider ID 与能力 Profile 不一致");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kStopped && state_ != VoiceSessionState::kFailed) {
            return Status::Error(ErrorCode::kConflict, "语音会话已经启动");
        }
        config_ = config;
        audio_formats_ = {.capture = config.audio, .playback = config.audio};
        audio_ready_ = false;
        state_ = VoiceSessionState::kStarting;
        response_armed_ = false;
        first_tts_audio_pending_ = false;
        awaiting_final_asr_ = false;
        interrupt_fence_pending_ = false;
        generation_++;
        config_.generation = generation_;
        next_sequence_ = 0;
    }
    provider_.SetAudioSink([this](AudioFrame frame) { return HandleAudio(std::move(frame)); });
    VoiceSessionConfig provider_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider_config = config_;
    }
    Status status = provider_.Connect(provider_config, [this](const VoiceEvent& event) { HandleEvent(event); });
    if (!status.ok()) {
        provider_.SetAudioSink({});
        (void)provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("provider_connect_failed", status.message);
        return status;
    }
    auto negotiated = provider_.audio_formats();
    if (!negotiated.ok() || !negotiated.value.has_value() || !negotiated.value->valid()) {
        provider_.SetAudioSink({});
        provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        const Status failure = negotiated.ok()
                                   ? Status::Error(ErrorCode::kInvalidArgument, "Provider 返回的双向音频格式无效")
                                   : negotiated.status;
        Emit("audio_negotiation_failed", failure.message);
        return failure;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_formats_ = *negotiated.value;
    }
    input_.SetAudioSink([this](AudioFrame frame) { return HandleInputAudio(std::move(frame)); });
    status = input_.Open(negotiated.value->capture);
    if (!status.ok()) {
        input_.SetAudioSink({});
        provider_.SetAudioSink({});
        provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("input_open_failed", status.message);
        return status;
    }
    status = output_.Open(negotiated.value->playback);
    if (!status.ok()) {
        input_.SetAudioSink({});
        provider_.SetAudioSink({});
        input_.Close();
        provider_.Disconnect();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("output_open_failed", status.message);
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_ready_ = true;
        state_ = VoiceSessionState::kReady;
    }
    Emit("ready", provider_.capabilities().provider_id);
    return Status::Ok();
}

void VoiceSession::HandleEvent(const VoiceEvent& event) {
    bool stale = false;
    bool generation_changed = false;
    bool disconnected = false;
    bool playback_aborted = false;
    bool interrupt_fence_reached = false;
    bool stop_input_for_tts = false;
    Status flush_status = Status::Ok();
    uint64_t generation = 0;
    std::string pending_wake_word;
    std::string pending_text_response;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A zero generation is not a wildcard: late events must not mutate a
        // session after interrupt or stop invalidated the old epoch.
        if (event.generation != generation_) {
            stale = true;
        } else if (event.kind == VoiceEventKind::kDisconnected && audio_ready_ &&
                   state_ != VoiceSessionState::kStopped && state_ != VoiceSessionState::kFailed) {
            ++generation_;
            config_.generation = generation_;
            next_sequence_ = 0;
            state_ = VoiceSessionState::kStarting;
            generation = generation_;
            generation_changed = true;
            disconnected = true;
            response_armed_ = false;
            awaiting_final_asr_ = false;
            interrupt_fence_pending_ = false;
            pending_interrupt_wake_word_.clear();
            pending_interrupt_text_response_.clear();
        } else if (event.kind == VoiceEventKind::kAsrText) {
            // 只接受仍处于本轮采集的 STT。服务器可能在 TTS 已开始后才
            // 送达上一段识别结果；该事件不得穿透到交互状态机重启“处理”。
            // kToolCall 不武装：启动/重连时的 MCP 发现消息（tools/list）并非
            // 用户本轮输入，不能提前放行服务端 TTS。
            if (state_ != VoiceSessionState::kCapturing &&
                !(state_ == VoiceSessionState::kReady && awaiting_final_asr_)) {
                stale = true;
            } else {
                response_armed_ = true;
                awaiting_final_asr_ = false;
            }
        } else if (event.kind == VoiceEventKind::kConnected && audio_ready_ && state_ == VoiceSessionState::kStarting) {
            state_ = VoiceSessionState::kReady;
        } else if (event.kind == VoiceEventKind::kTtsStarted) {
            // 仅接受本轮请求产生的 TTS：必须先收到有效 STT/工具调用（response_armed_）。
            // 允许 kReady（listen.stop 后最终 STT 到达、Session 已回 kReady 的回应路径）、
            // kCapturing、kThinking、kSpeaking。空闲且无本轮输入（未 armed）的残留 TTS
            // 一律忽略，避免设备在没有用户输入时擅自播报。
            const bool armed = response_armed_ || state_ == VoiceSessionState::kSpeaking;
            if (interrupt_fence_pending_ || !armed || state_ == VoiceSessionState::kStopped ||
                state_ == VoiceSessionState::kStarting || state_ == VoiceSessionState::kFailed) {
                stale = true;
            } else {
                // This board has no AEC path. Stop capture before accepting the
                // server's TTS binary frames, rather than running I2S RX/TX as a
                // misleading full-duplex conversation.
                stop_input_for_tts = state_ == VoiceSessionState::kCapturing;
                state_ = VoiceSessionState::kSpeaking;
                first_tts_audio_pending_ = true;
            }
        } else if (event.kind == VoiceEventKind::kTtsStopped) {
            if (interrupt_fence_pending_) {
                // 已先 Flush 且 response_armed_=false；这条终止标记是旧流所有
                // 在途音频均已越过 WebSocket 顺序边界的唯一依据。
                interrupt_fence_pending_ = false;
                response_armed_ = false;
                awaiting_final_asr_ = false;
                playback_aborted = true;
                interrupt_fence_reached = true;
                pending_wake_word = std::move(pending_interrupt_wake_word_);
                pending_text_response = std::move(pending_interrupt_text_response_);
            } else if (event.aborted && audio_ready_) {
                ++generation_;
                config_.generation = generation_;
                next_sequence_ = 0;
                state_ = VoiceSessionState::kReady;
                generation = generation_;
                playback_aborted = true;
                response_armed_ = false;
            } else if (state_ == VoiceSessionState::kSpeaking) {
                state_ = VoiceSessionState::kReady;
                // The response that armed this turn has completed.  Retaining
                // it would allow a delayed, unrelated tts.start to restart
                // playback while the session is otherwise idle.
                response_armed_ = false;
            }
        }
    }
    if (stale) {
        Emit("stale_event_dropped", "provider event generation mismatch");
    } else if (generation_changed) {
        provider_.SetGeneration(generation);
        Emit("transport_disconnected", "audio sending disabled until a new hello completes");
    } else if (playback_aborted) {
        // interrupt fence 只证明旧 tts.stop 已越过传输顺序边界。generation
        // 已在 Interrupt* 中设为新轮次，不能把 Provider 错设回局部默认值 0。
        if (!interrupt_fence_reached) {
            provider_.SetGeneration(generation);
        }
        flush_status = output_.Flush();
        Emit("tts_aborted", "server abort invalidated buffered playback");
        if (!flush_status.ok()) {
            Emit("playback_flush_failed", flush_status.message);
        }
    } else if (event.kind == VoiceEventKind::kConnected) {
        Emit("transport_connected", "provider hello completed");
    } else if (event.kind == VoiceEventKind::kAsrText) {
        Emit("stt_text_received", event.text);
    } else if (event.kind == VoiceEventKind::kToolCall) {
        Emit("tool_call_received", event.text);
    } else if (event.kind == VoiceEventKind::kTtsStarted) {
        if (stop_input_for_tts) {
            const Status status = input_.StopCapture();
            if (!status.ok()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (generation_ == event.generation && state_ == VoiceSessionState::kSpeaking) {
                        state_ = VoiceSessionState::kFailed;
                    }
                }
                Emit("tts_capture_stop_failed", status.message);
                return;
            }
        }
        Emit("tts_started", "");
    } else if (event.kind == VoiceEventKind::kTtsSentenceStarted) {
        // 仅当处于播报状态（本轮 TTS 已被 kTtsStarted 接受）时才回显句子；
        // 空闲态残留 TTS（如服务端闲聊）不显示文本。
        if (state_ == VoiceSessionState::kSpeaking) {
            Emit("tts_sentence_started", event.text);
        }
    } else if (event.kind == VoiceEventKind::kTtsStopped) {
        Emit("tts_stopped", "");
    } else if (event.kind == VoiceEventKind::kError) {
        Emit("provider_error", event.text);
    }
    if (!pending_wake_word.empty()) {
        // 旧流已返回 tts.stop；此时才能让 Provider 接收下一条确认播报。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != VoiceSessionState::kReady || !audio_ready_) {
                pending_wake_word.clear();
                pending_text_response.clear();
            } else {
                response_armed_ = !pending_text_response.empty();
            }
        }
        if (!pending_wake_word.empty()) {
            const Status status = provider_.NotifyLocalWakeWord(pending_wake_word, pending_text_response);
            if (!status.ok()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    response_armed_ = false;
                }
                Emit("interrupt_ack_failed", status.message);
            } else {
                Emit("interrupt_ack_requested", "");
            }
        }
    }
    if (disconnected) {
        return;
    }
}

Status VoiceSession::BeginCapture() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    VoiceMode mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kReady) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能开始采集");
        }
        mode = config_.mode;
    }
    Status provider_status = provider_.StartCapture(mode);
    if (!provider_status.ok()) {
        return provider_status;
    }
    Status input_status = input_.StartCapture(mode);
    if (input_status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kCapturing;
            next_sequence_ = 0;
            // 新回合开始：清零上一轮武装标记与 VAD 状态，只允许本轮有效输入武装回复。
            response_armed_ = false;
            vad_speech_seen_ = false;
            vad_speech_started_emitted_ = false;
            vad_silence_emitted_ = false;
            vad_silence_pending_ = false;
            uplink_failure_emitted_ = false;
            last_speech_at_ = {};
            awaiting_final_asr_ = false;
        }
        Emit("capture_started", "");
        return Status::Ok();
    }
    // Input failed after the provider already started listening. The provider
    // must be stopped so the server does not stay in a half-open capture state.
    Status rollback = provider_.StopCapture();
    if (!rollback.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("capture_rollback_failed", rollback.message);
        return rollback;
    }
    return input_status;
}

Status VoiceSession::EndCapture() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前没有采集");
        }
    }
    Status input_status = input_.StopCapture();
    Status provider_status = provider_.StopCapture();
    if (input_status.ok() && provider_status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kReady;
            awaiting_final_asr_ = true;
        }
        Emit("capture_stopped", "");
        return Status::Ok();
    }
    // Input is already stopped but provider stop failed (e.g. the WebSocket was
    // torn down mid-turn). The transport auto-reconnects and hello re-arms the
    // session, so keep the session usable for the next wake word instead of
    // stranding it in kFailed. Invalidate the old turn generation so any
    // residual TTS frames or stale listen state from the failed round are
    // rejected, and the next BeginCapture starts a fresh turn.
    // 注意：本地输入已停止，会话可复用，必须返回成功——调用方若看到失败会
    // 映射 kFailure 把控制器拖进 Error，导致后续无法恢复聆听。
    if (input_status.ok()) {
        uint64_t next_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            config_.generation = generation_;
            next_sequence_ = 0;
            next_generation = generation_;
            state_ = VoiceSessionState::kReady;
            awaiting_final_asr_ = false;
        }
        provider_.SetGeneration(next_generation);
        Emit("capture_stopped", "");
#ifdef ESP_PLATFORM
        ESP_LOGW("VoiceLifeVoiceSession",
                 "EndCapture: input stopped, provider stop 失败（可恢复，generation=%llu）: %s",
                 static_cast<unsigned long long>(next_generation), provider_status.message.c_str());
#endif
        return Status::Ok();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = VoiceSessionState::kFailed;
    }
    const Status failure = !input_status.ok() ? input_status : provider_status;
    Emit("capture_stop_failed", failure.message);
    return failure;
}

Status VoiceSession::SubmitAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!AcceptFrameLocked(frame)) {
            return Status::Error(ErrorCode::kInvalidArgument, "音频帧属于旧会话、乱序或格式无效");
        }
        if (frame.sequence != next_sequence_) {
            return Status::Error(ErrorCode::kConflict, "音频帧序号不连续");
        }
    }
    Status status = provider_.SendAudio(frame);
    if (status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++next_sequence_;
    }
    return status;
}

Status VoiceSession::HandleInputAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    uint64_t generation = 0;
    uint64_t sequence = 0;
    bool speech_started = false;
    bool silence_detected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能发送采集音频");
        }
        const AudioFormat& expected = audio_formats_.capture;
        if (!frame.format.valid() || frame.format.codec != expected.codec ||
            frame.format.sample_rate_hz != expected.sample_rate_hz || frame.format.channels != expected.channels ||
            frame.format.bits_per_sample != expected.bits_per_sample ||
            frame.format.frame_duration_ms != expected.frame_duration_ms || frame.payload.empty() ||
            frame.payload.size() > AudioFrame::kMaxPayloadBytes) {
            return Status::Error(ErrorCode::kInvalidArgument, "采集帧格式与会话不一致");
        }
        generation = generation_;
        sequence = next_sequence_;
        frame.generation = generation;
        frame.sequence = sequence;
        // 本地 VAD 端点（无 AFE，用 RMS 能量近似，PCM 已右移 14 位幅度较小）：
        // 带迟滞：进入语音用较高阈值，一旦检测到语音后以更低阈值维持，静音超
        // 1200ms 发一次 vad_silence 供上层进入最终 STT 等待。
        const auto* pcm = reinterpret_cast<const int16_t*>(frame.payload.data());
        const std::size_t sample_count = frame.payload.size() / sizeof(int16_t);
        int64_t sum = 0;
        for (std::size_t i = 0; i < sample_count; ++i) {
            sum += pcm[i];
        }
        const int64_t mean = sample_count > 0 ? sum / static_cast<int64_t>(sample_count) : 0;
        int64_t energy = 0;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const int64_t sample = static_cast<int64_t>(pcm[i]) - mean;
            energy += sample * sample;
        }
        const int64_t rms = sample_count > 0 ? energy / static_cast<int64_t>(sample_count) : 0;
        constexpr int64_t kSpeechEnterThreshold = 300 * 300;  // 进入语音约 -40 dBFS
        constexpr int64_t kSpeechExitThreshold = 180 * 180;   // 迟滞下限约 -44 dBFS
        const auto now = std::chrono::steady_clock::now();
        if (rms >= kSpeechEnterThreshold || (vad_speech_seen_ && rms >= kSpeechExitThreshold)) {
            speech_started = !vad_speech_seen_ && !vad_speech_started_emitted_;
            vad_speech_seen_ = true;
            vad_speech_started_emitted_ = true;
            last_speech_at_ = now;
        } else if (vad_speech_seen_ && !vad_silence_emitted_ && last_speech_at_.time_since_epoch().count() > 0 &&
                   now - last_speech_at_ >= std::chrono::milliseconds(1200)) {
            // 只在锁内置标志，锁外再 Emit，避免 Emit 重入 mutex_ 造成自死锁。
            vad_silence_emitted_ = true;
            vad_silence_pending_ = true;
            silence_detected = true;
        }
    }
    if (speech_started) {
        Emit("vad_speech_started", "");
    }
    if (silence_detected) {
        vad_silence_pending_ = false;
        Emit("vad_silence", "");
    }
    Status status = provider_.SendAudio(frame);
    if (status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == VoiceSessionState::kCapturing && generation_ == generation && next_sequence_ == sequence) {
            ++next_sequence_;
        }
    } else {
        bool first_failure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!uplink_failure_emitted_) {
                uplink_failure_emitted_ = true;
                first_failure = true;
            }
        }
        if (first_failure) {
            Emit("uplink_send_failed", status.message);
        }
    }
    return status;
}

Status VoiceSession::HandleAudio(AudioFrame frame) {
    bool first_audio = false;
    Status status = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kSpeaking) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播放音频");
        }
        const AudioFormat& expected = audio_formats_.playback;
        if (frame.generation != generation_ || frame.format.codec != expected.codec ||
            frame.format.sample_rate_hz != expected.sample_rate_hz || frame.format.channels != expected.channels ||
            frame.format.bits_per_sample != expected.bits_per_sample ||
            frame.format.frame_duration_ms != expected.frame_duration_ms || frame.payload.empty() ||
            frame.payload.size() > AudioFrame::kMaxPayloadBytes) {
            return Status::Error(ErrorCode::kInvalidArgument, "播放帧不属于当前会话");
        }
        status = output_.Push(frame);
        if (status.ok() && first_tts_audio_pending_) {
            first_tts_audio_pending_ = false;
            first_audio = true;
        }
    }
    if (first_audio) Emit("tts_first_audio", "");
    return status;
}

void VoiceSession::ReportToolCallStarted() { Emit("mcp_tool_started", ""); }

void VoiceSession::ReportToolResult(std::string_view summary, bool success) {
    Emit(success ? "mcp_tool_result" : "mcp_tool_failed", summary);
}

Status VoiceSession::Speak(std::string_view text) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kReady || text.empty()) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能播报");
        }
        // Speak only requests remote synthesis. The provider's tts.start is
        // the authoritative signal that PCM playback actually began.
        response_armed_ = true;
    }
    Status status = provider_.Speak(text);
    if (!status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        response_armed_ = false;
    }
    return status;
}

Status VoiceSession::NotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kReady || wake_word.empty()) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能通知本地唤醒");
        }
        // A text_response is a provider-requested system utterance. Arm only
        // that response; unrelated idle TTS remains rejected by HandleEvent.
        response_armed_ = !text_response.empty();
    }
    Status status = provider_.NotifyLocalWakeWord(wake_word, text_response);
    if (!status.ok() && !text_response.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        response_armed_ = false;
    } else if (status.ok()) {
        Emit("local_wake_ack_requested", "");
    }
    return status;
}

Status VoiceSession::InterruptAndNotifyLocalWakeWord(std::string_view wake_word, std::string_view text_response) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (wake_word.empty() || text_response.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "打断确认的唤醒词和确认文本不能为空");
    }
    bool capturing = false;
    bool wait_for_old_tts_stop = false;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing && state_ != VoiceSessionState::kSpeaking) {
            return Status::Error(ErrorCode::kUnavailable, "语音会话当前不能打断并确认");
        }
        capturing = state_ == VoiceSessionState::kCapturing;
        wait_for_old_tts_stop = state_ == VoiceSessionState::kSpeaking;
        ++generation_;
        config_.generation = generation_;
        next_sequence_ = 0;
        state_ = VoiceSessionState::kReady;
        response_armed_ = false;
        awaiting_final_asr_ = false;
        interrupt_fence_pending_ = wait_for_old_tts_stop;
        pending_interrupt_wake_word_.assign(wake_word);
        pending_interrupt_text_response_.assign(text_response);
        generation = generation_;
    }
    provider_.SetGeneration(generation);
    const Status input_status = capturing ? input_.StopCapture() : Status::Ok();
    const Status abort_status = provider_.Abort("user_interrupt");
    const Status flush_status = output_.Flush();
    if (!input_status.ok() || !abort_status.ok() || !flush_status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupt_fence_pending_ = false;
        pending_interrupt_wake_word_.clear();
        pending_interrupt_text_response_.clear();
        response_armed_ = false;
        return !input_status.ok() ? input_status : (!abort_status.ok() ? abort_status : flush_status);
    }
    Emit("interrupted", "old audio generation invalidated");
    if (!wait_for_old_tts_stop) {
        // 采集回合没有旧 TTS 收尾；可立即请求一次确认播报。
        std::string immediate_wake_word;
        std::string immediate_text_response;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            immediate_wake_word = std::move(pending_interrupt_wake_word_);
            immediate_text_response = std::move(pending_interrupt_text_response_);
            response_armed_ = true;
        }
        const Status status = provider_.NotifyLocalWakeWord(immediate_wake_word, immediate_text_response);
        if (!status.ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            response_armed_ = false;
            return status;
        }
        Emit("interrupt_ack_requested", "");
    }
    return Status::Ok();
}

Status VoiceSession::Interrupt() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    bool capturing = false;
    bool needs_interrupt_fence = false;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != VoiceSessionState::kCapturing && state_ != VoiceSessionState::kSpeaking) {
            return Status::Ok();
        }
        capturing = state_ == VoiceSessionState::kCapturing;
        needs_interrupt_fence = state_ == VoiceSessionState::kSpeaking;
        // Invalidate the old generation first. Late frames that arrive during
        // the subsequent Abort/Flush window are rejected by HandleAudio (which
        // checks generation_) and HandleInputAudio (which checks state_).
        ++generation_;
        config_.generation = generation_;
        next_sequence_ = 0;
        state_ = VoiceSessionState::kReady;
        // Abort invalidates both the transport generation and any TTS request
        // that authorized it. A delayed tts.start must not resurrect the
        // cancelled turn under the new generation.
        response_armed_ = false;
        awaiting_final_asr_ = false;
        interrupt_fence_pending_ = needs_interrupt_fence;
        pending_interrupt_wake_word_.clear();
        pending_interrupt_text_response_.clear();
        generation = generation_;
    }
    // Notify the provider before tearing down so it can drop stale frames.
    provider_.SetGeneration(generation);
    Status input_status = Status::Ok();
    if (capturing) {
        input_status = input_.StopCapture();
    }
    Status abort_status = provider_.Abort("user_interrupt");
    Status flush_status = output_.Flush();
    Emit("interrupted", "old audio generation invalidated");
    if (!input_status.ok()) return input_status;
    if (!abort_status.ok()) return abort_status;
    // 打断采集时并没有旧 TTS 流需要等待；立即解除后续确认播报的围栏。
    if (!needs_interrupt_fence) {
        Emit("interrupt_fence_reached", "no active playback");
    }
    return flush_status;
}

Status VoiceSession::Stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == VoiceSessionState::kStopped) {
            return Status::Ok();
        }
        ++generation_;
        config_.generation = generation_;
        next_sequence_ = 0;
        audio_ready_ = false;
        state_ = VoiceSessionState::kStopped;
        response_armed_ = false;
        awaiting_final_asr_ = false;
        interrupt_fence_pending_ = false;
        pending_interrupt_wake_word_.clear();
        pending_interrupt_text_response_.clear();
        generation = generation_;
    }
    provider_.SetGeneration(generation);
    provider_.SetAudioSink({});
    Status provider_status = provider_.Disconnect();
    input_.SetAudioSink({});
    // Stop every capture callback before the output device tears down a shared
    // duplex codec/I2S pair. Ports remain platform-neutral; the ordering only
    // expresses the session ownership contract.
    input_.Close();
    output_.Close();
    if (!provider_status.ok()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = VoiceSessionState::kFailed;
        }
        Emit("stop_disconnect_failed", provider_status.message);
    } else {
        Emit("stopped", "");
    }
    return provider_status;
}

}  // namespace voicelife::voice
