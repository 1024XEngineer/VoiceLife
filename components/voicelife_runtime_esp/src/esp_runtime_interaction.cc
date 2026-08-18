#include "esp_runtime_internal.h"

#ifdef ESP_PLATFORM
#include <cmath>
#include <ctime>

#include "esp_log.h"
#include "esp_timer.h"
#include "im_binding_mcp_tools.h"
#include "linx_mcp_bridge.h"

namespace voicelife::runtime {
std::string_view Runtime::PhaseStatusText(voice::VoiceInteractionState state) {
    switch (state) {
        case voice::VoiceInteractionState::kBooting:
            return "开机";
        case voice::VoiceInteractionState::kStandby:
            return "空闲";
        case voice::VoiceInteractionState::kOpeningCapture:
            return "聆听中";  // 采集请求提交中（事务式启动过渡）
        case voice::VoiceInteractionState::kListening:
            return "聆听中";
        case voice::VoiceInteractionState::kFinalizing:
            return "聆听中";  // 等待最终 STT，仍显示聆听
        case voice::VoiceInteractionState::kThinking:
            return "处理中";
        case voice::VoiceInteractionState::kSpeaking:
            return "说话中";
        case voice::VoiceInteractionState::kInterrupting:
            return "停止";
        case voice::VoiceInteractionState::kReconnecting:
            return "重连中";
        case voice::VoiceInteractionState::kError:
            return "出错了";
    }
    return "出错了";
}

voice::VoiceMood Runtime::PhaseMood(voice::VoiceInteractionState state) {
    switch (state) {
        case voice::VoiceInteractionState::kBooting:
            return voice::VoiceMood::kBooting;
        case voice::VoiceInteractionState::kStandby:
            return voice::VoiceMood::kIdle;
        case voice::VoiceInteractionState::kOpeningCapture:
        case voice::VoiceInteractionState::kListening:
        case voice::VoiceInteractionState::kFinalizing:
            return voice::VoiceMood::kListening;
        case voice::VoiceInteractionState::kThinking:
            return voice::VoiceMood::kThinking;
        case voice::VoiceInteractionState::kSpeaking:
            return voice::VoiceMood::kSpeaking;
        case voice::VoiceInteractionState::kInterrupting:
            return voice::VoiceMood::kCancelled;
        case voice::VoiceInteractionState::kReconnecting:
            return voice::VoiceMood::kConnecting;
        case voice::VoiceInteractionState::kError:
            return voice::VoiceMood::kSad;
    }
    return voice::VoiceMood::kSad;
}

std::string Runtime::CurrentStandbyStatusText() {
    const time_t now = time(nullptr);
    if (now <= 1600000000) return "空闲";  // 2020-09-13 之前视为尚未同步时钟。
    std::tm local{};
    localtime_r(&now, &local);
    char clock_text[8] = {};
    std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local.tm_hour, local.tm_min);
    return clock_text;
}

void Runtime::CommitSnapshot() {
    if (snapshot_.revision == last_rendered_revision_) {
        return;
    }
    last_rendered_revision_ = snapshot_.revision;
    // 显示语义通过 PresentationPort 提交；渲染由板级 Adapter 完成。
    if (assembly_ != nullptr) {
        (void)assembly_->presentation().Render(snapshot_);
    }
    ESP_LOGI(kTag,
             "INTERACTION_SNAPSHOT phase=%d generation=%llu revision=%llu mood=%d status_bytes=%u role=%d "
             "content_bytes=%u",
             static_cast<int>(snapshot_.phase), static_cast<unsigned long long>(snapshot_.generation),
             static_cast<unsigned long long>(snapshot_.revision), static_cast<int>(snapshot_.mood),
             static_cast<unsigned>(snapshot_.status_text.size()), static_cast<int>(snapshot_.role),
             static_cast<unsigned>(snapshot_.content_text.size()));
}

// 显示语义提交：只投递给 InteractionEventLoop，禁止在调用线程直接 Render。
void Runtime::ShowDisplay(voice::VoiceMood mood, std::string_view status, std::string_view content) {
    EnqueueDisplayUpdate(mood, status, content, false);
}

// 临时 overlay 快照：由事件循环统一写入，revision 与业务快照保持严格单调。
void Runtime::ShowOverlay(voice::VoiceMood mood, std::string_view status, std::string_view content) {
    EnqueueDisplayUpdate(mood, status, content, true);
}

void Runtime::StartOverlayTimer(uint32_t duration_ms) {
    volume_overlay_until_us_ = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    if (volume_overlay_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &VolumeOverlayEntry;
        args.arg = this;
        args.name = "voicelife_overlay";
        (void)esp_timer_create(&args, &volume_overlay_timer_);
    }
    if (volume_overlay_timer_ != nullptr) {
        (void)esp_timer_stop(volume_overlay_timer_);
        (void)esp_timer_start_once(volume_overlay_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL);
    }
}

// “收到！”是唤醒确认的短暂显示。即使服务端暂时没有后续语音事件，
// 也必须由事件循环在租约到期后主动刷新，否则 OLED 会永久保留确认文本。
void Runtime::ClearExpiredWakeAck() {
    if (wake_ack_until_us_ == 0 || esp_timer_get_time() < wake_ack_until_us_) return;
    wake_ack_until_us_ = 0;
    if (snapshot_.phase != voice::VoiceInteractionState::kListening ||
        snapshot_.role != voice::VoiceContentRole::kSystem || snapshot_.content_text != "收到！") {
        return;
    }
    snapshot_.content_text.clear();
    snapshot_.role = voice::VoiceContentRole::kNone;
    ++snapshot_.revision;
    CommitSnapshot();
    ESP_LOGI(kTag, "WAKE_ACK_DISPLAY_EXPIRED=1");
}

Status Runtime::HandleInteractionEvent(voice::VoiceInteractionEvent event, std::string_view wake_word) {
    active_wake_word_.assign(wake_word);
    const Status status = interaction_task_host_.Submit({.voice_event = event}, *this);
    if (!status.ok()) {
        ESP_LOGW(kTag, "忽略乱序板端交互事件=%d: %s", static_cast<int>(event), status.message.c_str());
    }
    active_wake_word_.clear();
    return status;
}

Status Runtime::Submit(application::InteractionAction transition) {
    const voice::VoiceInteractionEvent event = transition.source;
    // 新回合事件递增语义代次：显示任务按 generation -> revision 丢弃迟到快照。
    switch (event) {
        case voice::VoiceInteractionEvent::kToggleChat:
        case voice::VoiceInteractionEvent::kPressDown:
        case voice::VoiceInteractionEvent::kWakeDetected:
        case voice::VoiceInteractionEvent::kInterruptAndAcknowledge:
            ++snapshot_.generation;
            // A fresh user turn must never inherit a farewell decision
            // from a disconnected or cancelled preceding turn.
            terminal_turn_ = false;
            binding_turn_awaiting_tts_completion_ = false;
            break;
        case voice::VoiceInteractionEvent::kInterruptRequested:
        case voice::VoiceInteractionEvent::kTransportDisconnected:
        case voice::VoiceInteractionEvent::kFailure:
            // These paths invalidate the current remote turn before its
            // normal TTS completion can safely decide the next UI state.
            terminal_turn_ = false;
            binding_turn_awaiting_tts_completion_ = false;
            break;
        default:
            break;
    }
    // 会话阶段 → 显示模型快照：状态栏文本 + 表情由阶段派生。
    snapshot_.phase = transition.state;
    snapshot_.mood = PhaseMood(snapshot_.phase);
    if (snapshot_.phase != voice::VoiceInteractionState::kStandby && binding_terminal_display_active_) {
        CancelBindingTerminalDisplay();
    }
    // 空闲态显示当前时间（若服务端时间已初始化），否则显示状态词。
    if (snapshot_.phase == voice::VoiceInteractionState::kStandby) {
        snapshot_.status_text = CurrentStandbyStatusText();
    } else {
        snapshot_.status_text = PhaseStatusText(snapshot_.phase);
    }
    // 事件驱动的内容角色切换：
    // - kIntentReceived（STT）：内容栏显示用户语音，角色 user
    // - kTtsStarted：内容栏保持/显示助手文本，角色 assistant
    // - 会话结束/回待机：清空内容栏
    // WakeAck 租约：唤醒后短窗（400ms）内显示“收到！”，不阻塞开麦。
    if ((event == voice::VoiceInteractionEvent::kWakeDetected ||
         event == voice::VoiceInteractionEvent::kInterruptAndAcknowledge) &&
        snapshot_.phase == voice::VoiceInteractionState::kListening && wake_ack_until_us_ > 0 &&
        esp_timer_get_time() < wake_ack_until_us_) {
        snapshot_.content_text = "收到！";
        snapshot_.role = voice::VoiceContentRole::kSystem;
    } else if (event == voice::VoiceInteractionEvent::kEndpointDetected) {
        // VAD 端点：进入 kFinalizing 等待最终 STT，清掉“收到！”残留，
        // 显示“聆听中”状态词。
        wake_ack_until_us_ = 0;
        snapshot_.content_text.clear();
        snapshot_.role = voice::VoiceContentRole::kNone;
    } else if (event == voice::VoiceInteractionEvent::kIntentReceived && !stt_display_text_.empty()) {
        snapshot_.content_text = stt_display_text_;
        snapshot_.role = voice::VoiceContentRole::kUser;
    } else if (event == voice::VoiceInteractionEvent::kTtsStopped ||
               event == voice::VoiceInteractionEvent::kStandbyReady ||
               event == voice::VoiceInteractionEvent::kBootCompleted) {
        snapshot_.content_text.clear();
        snapshot_.role = voice::VoiceContentRole::kNone;
    }
    // 绑定码不是一帧临时字幕。普通语音回合可以覆盖它，但回到待机后必须
    // 恢复当前 pending 会话的六码与有效期，直到 Gateway 返回终态。
    if (snapshot_.phase == voice::VoiceInteractionState::kStandby && binding_display_active_ &&
        binding_display_generation_ == binding_use_case_.generation()) {
        snapshot_.mood = voice::VoiceMood::kNeutral;
        snapshot_.status_text = binding_status_text_;
        snapshot_.content_text = binding_content_text_;
        snapshot_.role = voice::VoiceContentRole::kSystem;
    }
    // 冗余 standby_ready 不得让绑定终态一闪而过；进入任何活跃状态
    // 会在上方取消租约，使新交互立即接管显示。
    if (snapshot_.phase == voice::VoiceInteractionState::kStandby && binding_terminal_display_active_) {
        snapshot_.mood = binding_terminal_mood_;
        snapshot_.status_text = binding_terminal_status_text_;
        snapshot_.content_text = binding_terminal_content_text_;
        snapshot_.role = voice::VoiceContentRole::kSystem;
    }
    ++snapshot_.revision;
    // 真实状态迁移优先于临时 overlay，过期信号不能恢复旧回合的 UI。
    overlay_active_ = false;
    CommitSnapshot();
    QueueDeferredBindingSpeechIfStandby();
    switch (transition.directive) {
        case voice::VoiceInteractionAction::kNone:
            return Status::Ok();
        case voice::VoiceInteractionAction::kStartCapture:
            QueueCaptureStart();
            return Status::Ok();
        case voice::VoiceInteractionAction::kStartVoiceTurn:
            if (active_wake_word_.empty()) {
                return Status::Error(ErrorCode::kInvalidArgument, "本地唤醒词不能为空");
            }
            QueueVoiceTurn(active_wake_word_);
            return Status::Ok();
        case voice::VoiceInteractionAction::kStopVoiceTurn:
            QueueCaptureStop();
            return Status::Ok();
        case voice::VoiceInteractionAction::kInterruptAndStartCapture:
            QueueInterruptAndCapture();
            return Status::Ok();
        case voice::VoiceInteractionAction::kInterruptAndStartVoiceTurn:
            if (active_wake_word_.empty()) {
                return Status::Error(ErrorCode::kInvalidArgument, "本地打断词不能为空");
            }
            QueueInterruptAndVoiceTurn(active_wake_word_);
            return Status::Ok();
        case voice::VoiceInteractionAction::kRestoreStandby:
            // transport_disconnected 必须停在 kReconnecting；物理唤醒门可恢复，
            // 但不可用 kStandbyReady 把可见状态提前伪装为空闲。
            QueueStandbyRecovery(transition.state != voice::VoiceInteractionState::kReconnecting);
            return Status::Ok();
        case voice::VoiceInteractionAction::kInterruptSession:
            QueueInterrupt();
            return Status::Ok();
    }
    return Status::Error(ErrorCode::kInternal, "未知板端交互动作");
}

void Runtime::LogVoiceEvidence(const voice::VoiceEvidence& evidence) { EnqueueVoiceEvidence(evidence); }

void Runtime::ProcessVoiceEvidence(const voice::VoiceEvidence& evidence) {
    // Evidence detail can contain STT text or service diagnostics. Emit
    // only lifecycle names and numeric counters needed for board review.
    if (evidence.event == "capture_started") {
        capture_started_us_.store(esp_timer_get_time());
        StartListenTimer(kListenStartTimeoutMs);
    }
    const int64_t started_at = capture_started_us_.load();
    const int64_t now = esp_timer_get_time();
    const uint64_t latency_ms =
        started_at > 0 && now >= started_at ? static_cast<uint64_t>((now - started_at) / 1000) : 0;
    if (assembly_ != nullptr) assembly_->LogAudioStats();
    ESP_LOGI(kTag, "VOICE_HEAP event=%s internal_free=%u internal_largest=%u psram_free=%u", evidence.event.c_str(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    ESP_LOGI(kTag, "VOICE_EVENT session=%s generation=%llu event=%s detail_present=%d latency_from_capture_ms=%llu",
             evidence.session_id.c_str(), static_cast<unsigned long long>(evidence.generation), evidence.event.c_str(),
             evidence.detail.empty() ? 0 : 1, static_cast<unsigned long long>(latency_ms));
    if (evidence.event == "provider_error") {
        // 板端诊断：只输出本地错误消息（不包含 STT 文本、凭据或原始响应）。
        ESP_LOGW(kTag, "PROVIDER_ERROR_DETAIL=%.160s", evidence.detail.c_str());
    }
    if (evidence.event == "tts_started" && wake_ack_requested_at_us_ > 0) {
        const int64_t wake_latency_ms = (esp_timer_get_time() - wake_ack_requested_at_us_) / 1000;
        if (wake_latency_ms >= 0 && wake_latency_ms <= 10000) {
            wake_ack_tts_started_at_us_ = esp_timer_get_time();
            ESP_LOGI(kTag, "WAKE_ACK_LATENCY stage=tts_started ms=%lld", static_cast<long long>(wake_latency_ms));
        }
    } else if (evidence.event == "tts_first_audio" && wake_ack_tts_started_at_us_ > 0) {
        const int64_t audio_latency_ms = (esp_timer_get_time() - wake_ack_requested_at_us_) / 1000;
        ESP_LOGI(kTag, "WAKE_ACK_LATENCY stage=first_audio ms=%lld", static_cast<long long>(audio_latency_ms));
    } else if (evidence.event == "tts_stopped" && wake_ack_tts_started_at_us_ > 0) {
        // 只关闭已经确认属于本次唤醒提示的计时窗口；后续回答的 TTS
        // 不得被误归类为首次确认时延。
        wake_ack_requested_at_us_ = 0;
        wake_ack_tts_started_at_us_ = 0;
    }
    if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted" || evidence.event == "provider_error" ||
        evidence.event == "capture_stop_failed" || evidence.event == "tts_capture_stop_failed") {
        capture_started_us_.store(0);
    }
    if (evidence.event == "capture_started") {
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kCaptureStarted);
    } else if (evidence.event == "stt_text_received") {
        // 收到用户语音转写（STT）：取消聆听超时，等待服务端回复。
        CancelListenTimer();
        // 抑制唤醒词被回传为 STT：唤醒后 1.5s 内收到等于唤醒词的文本，
        // 视为服务端把唤醒词误转写，不显示、不武装回复、不发 kIntentReceived。
        const bool wake_echo = !last_wake_word_.empty() && evidence.detail == last_wake_word_ &&
                               (last_wake_at_ > 0 && esp_timer_get_time() - last_wake_at_ < 1500 * 1000LL);
        if (wake_echo) {
            ESP_LOGI(kTag, "WAKE_ECHO_SUPPRESSED");
            // 中止该合成回合，避免服务端据此生成问候 TTS；随后由聆听超时/新输入重启。
            if (session_) {
                (void)session_->Interrupt();
            }
            return;
        }
        // 回写用户说的话到屏幕（detail 是 ASR 文本，属于用户自己的输入）。
        if (!evidence.detail.empty()) {
            stt_display_text_ = evidence.detail;
            // 终止意图识别：再见/拜拜/bye 等 → 播报结束后不 follow-up，直接收尾。
            terminal_turn_ =
                (evidence.detail.find("再见") != std::string::npos ||
                 evidence.detail.find("拜拜") != std::string::npos ||
                 evidence.detail.find("bye") != std::string::npos || evidence.detail.find("拜") != std::string::npos ||
                 evidence.detail.find("走了") != std::string::npos);
        }
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kIntentReceived);
        if (terminal_turn_) {
            // 不等待服务端针对“再见”的自由回复。先取消旧回合，再以 Linx
            // text_response 请求固定告别语，因此只会播放“牛牛走了～”。
            QueueSystemSpeech("牛牛走了～");
        }
    } else if (evidence.event == "tool_call_received") {
        // MCP 工具调用（服务端发现/工具执行）不是用户语音意图：
        // 仅取消聆听超时，不武装回复、不触发 kIntentReceived。
        CancelListenTimer();
    } else if (evidence.event == "mcp_tool_started") {
        // MCP worker 只经 VoiceSession evidence 投递；状态机决定是否允许
        // 从当前交互态进入“处理中”，不得由 worker 自己写快照。
        CancelListenTimer();
        const auto phase = interaction_orchestrator_.state();
        if (phase == voice::VoiceInteractionState::kListening || phase == voice::VoiceInteractionState::kFinalizing ||
            phase == voice::VoiceInteractionState::kThinking) {
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kIntentReceived);
        }
    } else if (evidence.event == "mcp_tool_result" || evidence.event == "mcp_tool_failed") {
        const bool success = evidence.event == "mcp_tool_result";
        // 绑定工具由 BindingPresentation 显示真实绑定码/终态。通用工具
        // overlay 不得用“日程操作已完成”等摘要覆盖绑定页面。
        if (IsBindingMcpToolSummary(evidence.detail)) {
            ESP_LOGI(kTag, "IM_BINDING_TOOL_OVERLAY_SUPPRESSED=1");
            return;
        }
        // evidence.detail 不是可信的用户文本。仅接受 MCP worker 产生的
        // 固定业务短句；任何原始 JSON-RPC/MCP 内容都降级为通用文案。
        std::string_view summary = success ? "操作已完成" : "操作失败";
        std::string_view status = success ? "操作结果" : "操作错误";
        if (success && evidence.detail == "日程已创建") {
            summary = "日程已创建";
            status = "日程结果";
        } else if (success && evidence.detail == "日程查询完成") {
            summary = "日程查询完成";
            status = "日程结果";
        } else if (!success && evidence.detail == "日程创建失败") {
            summary = "日程创建失败";
            status = "日程错误";
        } else if (!success && evidence.detail == "日程查询失败") {
            summary = "日程查询失败";
            status = "日程错误";
        }
        ShowOverlay(success ? voice::VoiceMood::kHappy : voice::VoiceMood::kSad, status, summary);
        StartOverlayTimer(2500);
    } else if (evidence.event == "tts_started") {
        CancelListenTimer();
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kTtsStarted);
    } else if (evidence.event == "local_wake_ack_requested" || evidence.event == "interrupt_ack_requested") {
        // 本地唤醒/打断确认已经成功提交给 Provider，但真正的 tts.start
        // 可能永远不到达（断线或服务端无响应）。此时 UI 已处于
        // kListening，必须有边界地回到待机，不能无限显示“聆听中”。
        StartListenTimer(kListenStartTimeoutMs);
    } else if (evidence.event == "tts_sentence_started") {
        // 回写服务端回复句子到屏幕（detail 为 TTS 文本），并立即提交快照
        // 让“说话中 + 助手文本”可见（不再停留显示用户 STT）。
        // 门控：仅当 Controller 已接受 kTtsStarted（处于 kSpeaking）才改显示；
        // 迟到的 TTS（Controller 已回 Standby/Error）直接丢弃，避免绕过状态机
        // 把屏幕卡在“说话中”。
        if (interaction_orchestrator_.state() != voice::VoiceInteractionState::kSpeaking) {
            ESP_LOGI(kTag, "TTS_SENTENCE_STALE state=%d 丢弃迟到句子",
                     static_cast<int>(interaction_orchestrator_.state()));
            return;
        }
        CancelListenTimer();
        if (!evidence.detail.empty()) {
            // 事件化：文本经事件循环应用（唯一写者），门控仍在事件循环校验。
            stt_display_text_ = evidence.detail;
            EnqueueDisplayText(evidence.detail);
        }
    } else if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted") {
        CancelListenTimer();
        // Provider disconnect/reconnect may deliver the completion of an
        // already-aborted remote TTS turn. It has no visible meaning once
        // the interaction loop has restored standby (or entered another
        // terminal state), so it must not re-enter the controller and
        // produce a false ordering error.
        if (interaction_orchestrator_.state() != voice::VoiceInteractionState::kSpeaking) {
            ESP_LOGI(kTag, "TTS_STOPPED_STALE state=%d 丢弃迟到结束事件",
                     static_cast<int>(interaction_orchestrator_.state()));
            return;
        }
        if (terminal_turn_ || binding_turn_awaiting_tts_completion_) {
            // 告别或绑定码播报完成后直接恢复待机。绑定码页面会在
            // HandleInteractionEvent 的待机呈现规则中立即恢复。
            terminal_turn_ = false;
            binding_turn_awaiting_tts_completion_ = false;
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kTerminalResponseCompleted);
        } else {
            // 事件化：kTtsStopped 由事件循环唯一执行状态迁移。
            EnqueueEvent(voice::VoiceInteractionEvent::kTtsStopped);
        }
    } else if (evidence.event == "transport_disconnected") {
        CancelListenTimer();
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kTransportDisconnected);
    } else if (evidence.event == "transport_connected") {
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kTransportConnected);
    } else if (evidence.event == "provider_error" || evidence.event == "capture_stop_failed" ||
               evidence.event == "tts_capture_stop_failed") {
        CancelListenTimer();
        // 会话已回待机后收到的 provider_error（如服务端有序 FIN/断开）是
        // 正常断线，不当作故障；随后的 transport_disconnected 走自动重连。
        // 仅会话进行中（聆听/处理/播报）的 provider_error 才算真正故障。
        const auto phase = interaction_orchestrator_.state();
        if (phase != voice::VoiceInteractionState::kStandby) {
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kFailure);
        }
    } else if (evidence.event == "capture_stopped") {
        // kFinalizing（等最终 STT）时不得取消 5s 最终 STT 定时器，
        // 否则服务端不返回 STT 时会永久悬挂；其余状态取消。
        if (interaction_orchestrator_.state() != voice::VoiceInteractionState::kFinalizing) {
            CancelListenTimer();
        }
    } else if (evidence.event == "vad_silence") {
        // 本地 VAD 端点：用户说完话后静音 1200ms，发 listen.stop 使服务端
        // 进入最终 STT，然后等待最终 STT（kFinalizing），不回待机。
        // 启动 5s 最终 STT 超时：无 STT 则 abort 收尾。
        CancelListenTimer();
        if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kListening) {
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kEndpointDetected);
            StartListenTimer(kFinalSttTimeoutMs);
        }
    }
}
}  // namespace voicelife::runtime
#endif
