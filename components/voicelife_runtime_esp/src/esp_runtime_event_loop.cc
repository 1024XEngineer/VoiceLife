#include "esp_runtime_internal.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "linx_ota_bootstrap.h"

namespace voicelife::runtime {
void Runtime::EnqueueEvent(voice::VoiceInteractionEvent event, std::string_view wake_word) {
    InteractionEventItem item{};
    item.event = event;
    item.wake_word = std::string(wake_word);
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) {
            event_queue_.pop_front();
        }
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

/** @brief 投递纯显示刷新（TTS 文本等，事件循环内应用，不触发状态机）。 */
void Runtime::EnqueueDisplayText(std::string detail) {
    InteractionEventItem item{};
    item.display_only = true;
    item.display_text = std::move(detail);
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) {
            event_queue_.pop_front();
        }
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

/** @brief 投递启动/错误/overlay 等系统语义；不携带硬件资源或原始数据。 */
void Runtime::EnqueueDisplayUpdate(voice::VoiceMood mood, std::string_view status, std::string_view content,
                                   bool overlay) {
    InteractionEventItem item{};
    item.display_update = true;
    item.display_overlay = overlay;
    item.display_mood = mood;
    item.display_status = std::string(status);
    item.display_content = std::string(content);
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

void Runtime::EnqueueBindingResult(const im::BindingResult& result) {
    InteractionEventItem item{};
    item.binding_result = true;
    item.binding = result;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

void Runtime::EnqueueBindingReset(uint64_t generation) {
    InteractionEventItem item{};
    item.binding_reset = true;
    item.binding_generation = generation;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

void Runtime::CancelBindingTerminalDisplay() {
    binding_terminal_display_active_ = false;
    binding_terminal_resume_listening_ = false;
    binding_terminal_until_us_ = 0;
    binding_terminal_status_text_.clear();
    binding_terminal_content_text_.clear();
}

void Runtime::ClearExpiredBindingTerminalDisplay() {
    if (!binding_terminal_display_active_ || binding_terminal_until_us_ == 0 ||
        esp_timer_get_time() < binding_terminal_until_us_) {
        return;
    }
    const bool resume_listening = binding_terminal_resume_listening_;
    CancelBindingTerminalDisplay();
    deferred_binding_speech_.clear();
    if (interaction_orchestrator_.state() != voice::VoiceInteractionState::kStandby) return;
    if (resume_listening) {
        ESP_LOGI(kTag, "IM_BINDING_TERMINAL_DISPLAY_EXPIRED=1 next=listening");
        snapshot_.content_text.clear();
        snapshot_.role = voice::VoiceContentRole::kNone;
        (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kToggleChat);
        return;
    }
    snapshot_.phase = voice::VoiceInteractionState::kStandby;
    snapshot_.mood = voice::VoiceMood::kIdle;
    snapshot_.status_text = CurrentStandbyStatusText();
    snapshot_.content_text.clear();
    snapshot_.role = voice::VoiceContentRole::kNone;
    ++snapshot_.revision;
    overlay_active_ = false;
    CommitSnapshot();
    ESP_LOGI(kTag, "IM_BINDING_TERMINAL_DISPLAY_EXPIRED=1");
}

void Runtime::CommitBindingPresentation(const BindingPresentation& presentation) {
    snapshot_.mood = presentation.content_text == "绑定成功" ? voice::VoiceMood::kHappy : voice::VoiceMood::kNeutral;
    snapshot_.status_text = presentation.status_text;
    snapshot_.content_text = presentation.content_text;
    snapshot_.role = voice::VoiceContentRole::kSystem;
    ++snapshot_.revision;
    overlay_active_ = false;
    CommitSnapshot();
    if (presentation.display_duration_ms > 0) {
        binding_terminal_display_active_ = true;
        binding_terminal_mood_ = snapshot_.mood;
        binding_terminal_status_text_ = presentation.status_text;
        binding_terminal_content_text_ = presentation.content_text;
        binding_terminal_resume_listening_ = presentation.resume_listening;
        binding_terminal_until_us_ =
            esp_timer_get_time() + static_cast<int64_t>(presentation.display_duration_ms) * 1000;
    } else {
        CancelBindingTerminalDisplay();
    }
}

void Runtime::QueueDeferredBindingSpeechIfStandby() {
    if (interaction_orchestrator_.state() != voice::VoiceInteractionState::kStandby) return;
    if (deferred_binding_presentation_.has_value()) {
        CommitBindingPresentation(*deferred_binding_presentation_);
        deferred_binding_presentation_.reset();
    }
    if (deferred_binding_speech_.empty()) return;
    std::string speech = std::move(deferred_binding_speech_);
    deferred_binding_speech_.clear();
    if (!QueueSystemSpeech(speech)) deferred_binding_speech_ = std::move(speech);
}

void Runtime::ProcessBindingResult(const im::BindingResult& result) {
    // Bind() increments the generation before replacing client/config dependencies.
    // A completed HTTP query from the prior origin can therefore never show success
    // after reconfiguration or an explicit restart.
    const uint64_t current_generation = binding_use_case_.generation();
    if (!IsCurrentBindingResult(result, current_generation)) {
        ESP_LOGI(kTag, "IM_BINDING_STALE_RESULT=1 result_generation=%llu current_generation=%llu",
                 static_cast<unsigned long long>(result.generation),
                 static_cast<unsigned long long>(current_generation));
        return;
    }
    const BindingPresentation presentation = PresentBindingResult(result);
    if (!presentation.keep_visible && !presentation.announce) return;

    if (ShouldEndVoiceTurnAfterBindingResult(
            result, interaction_orchestrator_.state() != voice::VoiceInteractionState::kStandby)) {
        binding_turn_awaiting_tts_completion_ = true;
    }

    binding_display_active_ = presentation.keep_visible;
    binding_display_generation_ = result.generation;
    if (presentation.keep_visible) {
        binding_status_text_ = presentation.status_text;
        binding_content_text_ = presentation.content_text;
    } else {
        binding_status_text_.clear();
        binding_content_text_.clear();
    }
    // 终态在普通对话中抵达时，将 OLED 与 TTS 作为一个结果延后到待机。
    // 这不会抢写用户正在看的 STT 或助手回复。
    if (!presentation.keep_visible && interaction_orchestrator_.state() != voice::VoiceInteractionState::kStandby) {
        deferred_binding_presentation_ = presentation;
        deferred_binding_speech_ = presentation.speech_text;
        return;
    }

    CommitBindingPresentation(presentation);
    if (!presentation.announce) return;
    if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kStandby) {
        if (!QueueSystemSpeech(presentation.speech_text)) deferred_binding_speech_ = presentation.speech_text;
    } else {
        // 活跃 MCP 回合的响应已携带 speak_text，由 Provider 播报一次。
        // 不再延迟本地重复播报；该播报结束后会直接回待机显示绑定码。
    }
}

void Runtime::EnqueueVoiceEvidence(const voice::VoiceEvidence& evidence) {
    InteractionEventItem item{};
    item.voice_evidence = true;
    item.evidence = evidence;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

void Runtime::EnqueueListenTimeout() {
    InteractionEventItem item{};
    item.listen_timeout = true;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

void Runtime::EnqueueNetworkState(bool connected) {
    InteractionEventItem item{};
    item.network_update = true;
    item.network_connected = connected;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

/** @brief 事件循环任务入口（唯一调用 HandleInteractionEvent 的线程）。 */
void Runtime::EventLoopTaskEntry(void* arg) { static_cast<Runtime*>(arg)->EventLoopLoop(); }

/** @brief 事件循环：消费事件 -> 状态迁移 -> 快照 -> 显示提交。 */
void Runtime::EventLoopLoop() {
#ifdef ESP_PLATFORM
    while (true) {
        InteractionEventItem item;
        {
            std::unique_lock<std::mutex> lock(event_mutex_);
            event_cv_.wait_for(lock, std::chrono::milliseconds(200),
                               [this] { return event_loop_stop_ || !event_queue_.empty(); });
            if (event_loop_stop_ && event_queue_.empty()) {
                break;
            }
            if (event_queue_.empty()) {
                // 超时轮询：处理短暂显示的到期刷新（不依赖 timer 直接提交）。
                ClearExpiredWakeAck();
                ClearExpiredBindingTerminalDisplay();
                if (overlay_expired_.exchange(false)) {
                    if (overlay_active_) {
                        snapshot_ = overlay_base_snapshot_;
                        ++snapshot_.revision;
                        overlay_active_ = false;
                        CommitSnapshot();
                    }
                }
                continue;
            }
            item = std::move(event_queue_.front());
            event_queue_.pop_front();
        }
        // provider_error 等事件持续占满队列时，终态租约仍必须按时收口。
        ClearExpiredBindingTerminalDisplay();
        if (item.display_only) {
            // 纯显示刷新：仅当控制器处于 kSpeaking 时应用（迟到的 TTS 丢弃）。
            if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kSpeaking &&
                !item.display_text.empty()) {
                snapshot_.content_text = item.display_text;
                snapshot_.role = voice::VoiceContentRole::kAssistant;
                snapshot_.status_text = "说话中";
                snapshot_.mood = voice::VoiceMood::kSpeaking;
                ++snapshot_.revision;
                CommitSnapshot();
            }
            continue;
        }
        if (item.network_update) {
            snapshot_.network_connected = item.network_connected;
            continue;
        }
        if (item.board_input) {
            switch (item.board_action) {
                case BoardInputAction::kToggleChat:
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kToggleChat);
                    break;
                case BoardInputAction::kPressDown:
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kPressDown);
                    break;
                case BoardInputAction::kPressUp:
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kPressUp);
                    break;
                case BoardInputAction::kVolumeUp:
                    SetVolume(std::min(volume_ + 10, 100));
                    break;
                case BoardInputAction::kVolumeDown:
                    SetVolume(std::max(volume_ - 10, 0));
                    break;
                case BoardInputAction::kVolumeMaximum:
                    SetVolume(100);
                    break;
                case BoardInputAction::kVolumeMute:
                    SetVolume(0);
                    break;
                case BoardInputAction::kStartWifiProvisioning: {
                    ShowDisplay(voice::VoiceMood::kConnecting, "配网", "正在开启热点");
                    const Status requested = RequestLinxWifiProvisioning();
                    if (!requested.ok()) ShowDisplay(voice::VoiceMood::kSad, "配网失败", "");
                    break;
                }
            }
            continue;
        }
        if (item.voice_evidence) {
            ProcessVoiceEvidence(item.evidence);
            continue;
        }
        if (item.binding_result) {
            ProcessBindingResult(item.binding);
            continue;
        }
        if (item.binding_reset) {
            if (item.binding_generation == binding_use_case_.generation()) {
                binding_display_active_ = false;
                binding_display_generation_ = item.binding_generation;
                binding_status_text_.clear();
                binding_content_text_.clear();
                deferred_binding_presentation_.reset();
                deferred_binding_speech_.clear();
                binding_turn_awaiting_tts_completion_ = false;
                CancelBindingTerminalDisplay();
                // 重绑/重启策略不允许旧 origin 的绑定码或成功提示留在屏幕上。
                // 非空闲回合会由紧随其后的交互事件接管显示；空闲时立即收口。
                if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kStandby) {
                    snapshot_.mood = voice::VoiceMood::kIdle;
                    snapshot_.status_text = CurrentStandbyStatusText();
                    snapshot_.content_text.clear();
                    snapshot_.role = voice::VoiceContentRole::kNone;
                    ++snapshot_.revision;
                    overlay_active_ = false;
                    CommitSnapshot();
                }
            }
            continue;
        }
        if (item.listen_timeout) {
            if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kListening) {
                // 实机麦克风底噪可能让本地 VAD 未能识别静音端点，但此前
                // 已采集的语音仍必须以 listen.stop 交给服务端完成最终 STT。
                // 直接 abort 会无条件丢弃该回合，表现为“收到后不再回应”。
                ESP_LOGI(kTag, "LISTEN_TIMEOUT transition=listening->finalizing");
                (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kEndpointDetected);
                StartListenTimer(kFinalSttTimeoutMs);
            } else if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kFinalizing) {
                ESP_LOGI(kTag, "FINALIZE_TIMEOUT transition=finalizing->standby");
                if (session_) (void)session_->Interrupt();
                (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFinalizationTimedOut);
            }
            continue;
        }
        if (item.display_update) {
            if (item.display_overlay) {
                overlay_base_snapshot_ = snapshot_;
                overlay_active_ = true;
            } else {
                overlay_active_ = false;
            }
            snapshot_.mood = item.display_mood;
            snapshot_.status_text = std::move(item.display_status);
            snapshot_.content_text = std::move(item.display_content);
            snapshot_.role = voice::VoiceContentRole::kSystem;
            ++snapshot_.revision;
            CommitSnapshot();
            ESP_LOGI(kTag, "DISPLAY_SEMANTIC_UPDATE overlay=%d mood=%d generation=%llu revision=%llu",
                     item.display_overlay ? 1 : 0, static_cast<int>(snapshot_.mood),
                     static_cast<unsigned long long>(snapshot_.generation),
                     static_cast<unsigned long long>(snapshot_.revision));
            continue;
        }
        if (item.event == voice::VoiceInteractionEvent::kWakeDetected ||
            item.event == voice::VoiceInteractionEvent::kInterruptAndAcknowledge) {
            // 唤醒前置（唯一状态写者内）：显示租约；声音由 Linx TTS 的
            // text_response 产生，绝不在 Runtime 直接推裸 PCM。
            last_wake_word_ = item.wake_word;
            last_wake_at_ = esp_timer_get_time();
            wake_ack_requested_at_us_ = last_wake_at_;
            wake_ack_tts_started_at_us_ = 0;
            wake_ack_until_us_ = esp_timer_get_time() + kWakeAckDisplayUs;
        }
        const Status wake_status = HandleInteractionEvent(item.event, item.wake_word);
        if (item.event != voice::VoiceInteractionEvent::kWakeDetected && !wake_status.ok()) {
            ESP_LOGW(kTag, "INTERACTION_REJECTED event=%d state=%d err=%s", static_cast<int>(item.event),
                     static_cast<int>(interaction_orchestrator_.state()), wake_status.message.c_str());
        }
        if ((item.event == voice::VoiceInteractionEvent::kWakeDetected ||
             item.event == voice::VoiceInteractionEvent::kInterruptAndAcknowledge ||
             item.event == voice::VoiceInteractionEvent::kInterruptRequested) &&
            !wake_status.ok()) {
            // 非法本地命令（例如待机时“别说了”）不得让检测器停死。
            ESP_LOGW(kTag, "LOCAL_COMMAND_REJECTED state=%d err=%s",
                     static_cast<int>(interaction_orchestrator_.state()), wake_status.message.c_str());
            if (assembly_->uses_local_wake_detector()) {
                (void)assembly_->wake_gate().StartStandby();
            }
        }
    }
    event_loop_stopped_ = true;
    vTaskDelete(nullptr);
#endif
}
}  // namespace voicelife::runtime
#endif
