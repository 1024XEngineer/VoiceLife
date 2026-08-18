#include "esp_runtime_internal.h"

#ifdef ESP_PLATFORM
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"

namespace voicelife::runtime {
void Runtime::EnqueueBoardInput(BoardInputAction action) {
    InteractionEventItem item{};
    item.board_input = true;
    item.board_action = action;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_.size() >= kEventQueueCapacity) event_queue_.pop_front();
        event_queue_.push_back(std::move(item));
    }
    event_cv_.notify_one();
}

void Runtime::SetVolume(int volume) {
    volume_ = std::clamp(volume, 0, 100);
    if (assembly_ != nullptr) assembly_->SetOutputVolume(volume_);
    // 音量通知 overlay：临时覆盖显示，1.5s 后恢复最新快照（不修改会话状态）。
    // 连续调音量只重置同一个计时器。
    char text[16] = {};
    std::snprintf(text, sizeof(text), "VOL:%d", volume_);
    ShowOverlay(voice::VoiceMood::kIdle, "音量", text);
    volume_overlay_until_us_ = esp_timer_get_time() + kVolumeOverlayUs;
    if (volume_overlay_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &VolumeOverlayEntry;
        args.arg = this;
        args.name = "voicelife_volume_overlay";
        (void)esp_timer_create(&args, &volume_overlay_timer_);
    }
    if (volume_overlay_timer_ != nullptr) {
        (void)esp_timer_stop(volume_overlay_timer_);
        (void)esp_timer_start_once(volume_overlay_timer_, kVolumeOverlayUs);
    }
}

void Runtime::QueueWakeWord(std::string_view wake_word) {
    LogVoiceEvidence({.session_id = session_ ? session_->config().session_id : "",
                      .generation = session_ ? session_->generation() : 0,
                      .event = "wake_detected",
                      .detail = {}});
    // “别说了”要中止旧播报后只回复一次“收到！”，随即转入聆听；它不是
    // 静默中止，也不能被当作普通唤醒后让旧 TTS 继续播放。
    const auto event = wake_word == "别说了" ? voice::VoiceInteractionEvent::kInterruptAndAcknowledge
                                             : voice::VoiceInteractionEvent::kWakeDetected;
    EnqueueEvent(event, wake_word);
}

void Runtime::QueueVoiceTurn(std::string_view wake_word) {
    if (wake_queue_ == nullptr) return;
    BoardRequest request{};
    request.kind = BoardRequestKind::kWakeWord;
    const std::size_t size =
        wake_word.size() < sizeof(request.wake_word) - 1 ? wake_word.size() : sizeof(request.wake_word) - 1;
    std::memcpy(request.wake_word, wake_word.data(), size);
    request.wake_word[size] = '\0';
    (void)xQueueSend(wake_queue_, &request, 0);
}

void Runtime::QueueInterruptAndVoiceTurn(std::string_view wake_word) {
    if (wake_queue_ == nullptr) return;
    BoardRequest request{};
    request.kind = BoardRequestKind::kInterruptAndWakeWord;
    const std::size_t size =
        wake_word.size() < sizeof(request.wake_word) - 1 ? wake_word.size() : sizeof(request.wake_word) - 1;
    std::memcpy(request.wake_word, wake_word.data(), size);
    request.wake_word[size] = '\0';
    (void)xQueueSend(wake_queue_, &request, 0);
}

void Runtime::QueueStandbyRecovery(bool settle_controller) {
    if (wake_queue_ == nullptr) return;
    BoardRequest recovery{};
    recovery.settle_controller = settle_controller;
    (void)xQueueSend(wake_queue_, &recovery, 0);
}

bool Runtime::QueueSystemSpeech(std::string_view text) {
    if (wake_queue_ == nullptr || text.empty()) return false;
    if (text.size() >= kBindingSystemSpeechCapacity) {
        ESP_LOGE(kTag, "SYSTEM_SPEECH_TOO_LONG bytes=%u", static_cast<unsigned>(text.size()));
        return false;
    }
    BoardRequest request{};
    request.kind = BoardRequestKind::kInterrupt;
    std::memcpy(request.system_speech, text.data(), text.size());
    request.system_speech[text.size()] = '\0';
    if (xQueueSend(wake_queue_, &request, 0) != pdTRUE) {
        ESP_LOGW(kTag, "SYSTEM_SPEECH_QUEUE_FULL=1");
        return false;
    }
    return true;
}

// 下行长文本滚动由显示 Adapter 负责（Ssd1306PresentationAdapter）。
// 音量 overlay 到期：递增 revision 触发 CommitSnapshot 恢复最新快照。
void Runtime::VolumeOverlayEntry(void* context) {
    auto* self = static_cast<Runtime*>(context);
    self->volume_overlay_until_us_ = 0;
    self->overlay_expired_.store(true);  // 只置标志；恢复由事件循环唯一执行。
}

// 聆听/最终 STT 超时：
// - kListening 超时（无有效输入）：结束本轮回待机
// - kFinalizing 超时（listen.stop 后 5s 无最终 STT）：abort 结束服务端回合回待机
void Runtime::ListenTimeoutEntry(void* context) {
    auto* self = static_cast<Runtime*>(context);
    // Timer 回调不能读取或迁移交互状态；由事件循环串行决定超时路径。
    ESP_LOGI(kTag, "LISTEN_TIMEOUT_FIRED");
    self->EnqueueListenTimeout();
}

void Runtime::StartListenTimer(uint32_t timeout_ms) {
    if (listen_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &ListenTimeoutEntry;
        args.arg = this;
        args.name = "voicelife_listen_timeout";
        if (esp_timer_create(&args, &listen_timer_) != ESP_OK) {
            listen_timer_ = nullptr;
            return;
        }
    }
    (void)esp_timer_stop(listen_timer_);
    const esp_err_t start = esp_timer_start_once(listen_timer_, timeout_ms * 1000ULL);
    if (start != ESP_OK) {
        ESP_LOGW(kTag, "LISTEN_TIMEOUT_ARM_FAILED ms=%u err=%d", static_cast<unsigned>(timeout_ms),
                 static_cast<int>(start));
        return;
    }
    ESP_LOGI(kTag, "LISTEN_TIMEOUT_ARMED ms=%u", static_cast<unsigned>(timeout_ms));
}

void Runtime::CancelListenTimer() {
    if (listen_timer_ != nullptr) {
        (void)esp_timer_stop(listen_timer_);
    }
}

void Runtime::QueueInterrupt() {
    if (wake_queue_ == nullptr) return;
    BoardRequest request{};
    request.kind = BoardRequestKind::kInterrupt;
    (void)xQueueSend(wake_queue_, &request, 0);
}

void Runtime::QueueCaptureStart() {
    if (wake_queue_ == nullptr) return;
    BoardRequest request{};
    request.kind = BoardRequestKind::kStartCapture;
    (void)xQueueSend(wake_queue_, &request, 0);
}

void Runtime::QueueCaptureStop() {
    if (wake_queue_ == nullptr) return;
    BoardRequest request{};
    request.kind = BoardRequestKind::kStopCapture;
    (void)xQueueSend(wake_queue_, &request, 0);
}

void Runtime::QueueInterruptAndCapture() {
    if (wake_queue_ == nullptr) return;
    BoardRequest request{};
    request.kind = BoardRequestKind::kInterruptAndStartCapture;
    (void)xQueueSend(wake_queue_, &request, 0);
}

#if CONFIG_VOICELIFE_STATE_FLOW_TEST
Status Runtime::StartStateFlowDiagnostic() {
    if (state_flow_task_ != nullptr) return Status::Ok();
    if (xTaskCreate(&Runtime::StateFlowTaskEntry, "voicelife_state_flow", 4096, this, 1, &state_flow_task_) != pdPASS) {
        return Status::Error(ErrorCode::kInternal, "创建状态流诊断任务失败");
    }
    ESP_LOGI(kTag, "STATE_FLOW_TEST_STARTED production_default=0");
    return Status::Ok();
}

void Runtime::StateFlowTaskEntry(void* context) { static_cast<Runtime*>(context)->StateFlowTask(); }

void Runtime::StateFlowEvent(uint32_t step, voice::VoiceInteractionEvent event) {
    ESP_LOGI(kTag, "STATE_FLOW_ENQUEUE step=%u kind=interaction event=%d", static_cast<unsigned>(step),
             static_cast<int>(event));
    EnqueueEvent(event);
}

void Runtime::StateFlowEvidence(uint32_t step, std::string_view event, std::string_view detail) {
    ESP_LOGI(kTag, "STATE_FLOW_ENQUEUE step=%u kind=evidence event=%.*s detail_bytes=%u", static_cast<unsigned>(step),
             static_cast<int>(event.size()), event.data(), static_cast<unsigned>(detail.size()));
    voice::VoiceEvidence evidence;
    evidence.session_id = session_ ? session_->config().session_id : "state-flow";
    evidence.generation = session_ ? session_->generation() : 0;
    evidence.event = std::string(event);
    evidence.detail = std::string(detail);
    EnqueueVoiceEvidence(evidence);
}

void Runtime::StateFlowTask() {
    // Test-only diagnostic. It submits normal semantic inputs/evidence and
    // never calls a renderer, PresentationPort, GPIO, or audio output.
    vTaskDelay(pdMS_TO_TICKS(1500));
    uint32_t step = 1;
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kTransportDisconnected);
    vTaskDelay(pdMS_TO_TICKS(350));
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kTransportConnected);
    vTaskDelay(pdMS_TO_TICKS(350));
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kPressDown);
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "capture_started");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "stt_text_received", "请在明天 09:30 创建日程: Review #42, room A-3.");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "mcp_tool_started");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "mcp_tool_result", "event=Review #42; status=created");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "tts_started");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "tts_sentence_started", "已创建日程。明天 09:30 在 A-3 开会。");
    vTaskDelay(pdMS_TO_TICKS(150));
    // A state-flow build must not invent a local TTS completion when no
    // real PCM turn was opened. Exercise the production cancellation path
    // instead: Runtime asks VoiceSession to interrupt and only its real
    // completion restores standby.
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kInterruptRequested);
    vTaskDelay(pdMS_TO_TICKS(500));
    for (uint32_t cycle = 0; cycle < 20; ++cycle) {
        StateFlowEvent(step++, voice::VoiceInteractionEvent::kTransportDisconnected);
        vTaskDelay(pdMS_TO_TICKS(90));
        StateFlowEvent(step++, voice::VoiceInteractionEvent::kTransportConnected);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kPressDown);
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "capture_started");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kFailure);
    vTaskDelay(pdMS_TO_TICKS(300));
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kStandbyReady);
    vTaskDelay(pdMS_TO_TICKS(300));
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kPressDown);
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvidence(step++, "capture_started");
    vTaskDelay(pdMS_TO_TICKS(150));
    StateFlowEvent(step++, voice::VoiceInteractionEvent::kInterruptRequested);
    vTaskDelay(pdMS_TO_TICKS(150));
    // kInterruptRequested reaches VoiceSession, whose real interrupted
    // evidence restores standby through the event loop. Do not inject a
    // second completion after that recovery: it is necessarily stale and
    // would make this diagnostic report a false ordering rejection.
    ESP_LOGI(kTag, "STATE_FLOW_TEST_FINISHED steps=%u", static_cast<unsigned>(step - 1));
    state_flow_task_ = nullptr;
    vTaskDelete(nullptr);
}
#endif

void Runtime::RestoreStandby(const BoardRequest& request) {
    if (assembly_ == nullptr) return;
    const Status stop_status = assembly_->wake_gate().StopCapture();
    if (!stop_status.ok()) {
        ESP_LOGW(kTag, "本地待机恢复停止上行失败: %s", stop_status.message.c_str());
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kFailure);
        return;
    }
    const Status standby_status = assembly_->wake_gate().StartStandby();
    if (!standby_status.ok()) {
        ESP_LOGW(kTag, "本地待机恢复失败: %s", standby_status.message.c_str());
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kFailure);
        return;
    }
    LogVoiceEvidence({.session_id = session_ ? session_->config().session_id : "",
                      .generation = session_ ? session_->generation() : 0,
                      .event = "standby_ready",
                      .detail = {}});
    // 显式派发 kStandbyReady：Controller 从 Error/kFinalizing 回 Standby，
    // 避免 RestoreStandby 直接写快照造成控制器仍停 Error 的假待机
    // （WAKE_REARM atomic=0）。Controller 回 Standby 后由状态机动作
    // 统一提交时间快照。
    // 事件化：状态迁移由事件循环唯一执行，拒绝日志在事件循环统一输出。
    if (request.settle_controller) {
        EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
    }
}

void Runtime::WakeTaskEntry(void* context) { static_cast<Runtime*>(context)->WakeTask(); }

void Runtime::WakeTask() {
    BoardRequest request{};
    while (true) {
        if (xQueueReceive(wake_queue_, &request, portMAX_DELAY) != pdTRUE) continue;
        if (request.kind == BoardRequestKind::kRestoreStandby) {
            RestoreStandby(request);
            continue;
        }
        if (request.kind == BoardRequestKind::kInterruptAndWakeWord) {
            if (!session_ || !provider_) continue;
            const Status acknowledge = session_->InterruptAndNotifyLocalWakeWord(request.wake_word, "收到！");
            if (!acknowledge.ok()) {
                ESP_LOGW(kTag, "打断确认请求失败: %s", acknowledge.message.c_str());
                QueueStandbyRecovery();
            }
            continue;
        }
        if (request.kind == BoardRequestKind::kInterrupt) {
            if (!session_) continue;
            const Status interrupt = session_->Interrupt();
            if (request.system_speech[0] != '\0') {
                const Status speak = interrupt.ok() ? session_->Speak(request.system_speech) : interrupt;
                if (!speak.ok()) {
                    ESP_LOGW(kTag, "系统播报请求失败: %s", speak.message.c_str());
                    QueueStandbyRecovery();
                }
                continue;
            }
            if (interrupt.ok()) {
                if (interaction_orchestrator_.state() == voice::VoiceInteractionState::kInterrupting) {
                    (void)EnqueueEvent(voice::VoiceInteractionEvent::kInterruptCompleted);
                } else {
                    QueueStandbyRecovery();
                }
            } else {
                ESP_LOGW(kTag, "板端打断失败: %s", interrupt.message.c_str());
                QueueStandbyRecovery();
            }
            continue;
        }
        if (request.kind == BoardRequestKind::kStartCapture) {
            // 开麦前等待播放排空（I2S 实际播完，而非队列空），避免把残留
            // TTS 重新采进 follow-up（NoAudioCodec 无 AEC）。
            if (assembly_ != nullptr) {
                for (int i = 0; i < 30 && !assembly_->audio_output().IsIdle(); ++i) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
            const Status capture =
                session_ ? session_->BeginCapture() : Status::Error(ErrorCode::kUnavailable, "语音会话尚未启动");
            if (!capture.ok()) {
                ESP_LOGW(kTag, "板级按键开始采集失败: %s", capture.message.c_str());
                // 事务式启动失败：回待机（kStandbyReady），不显示"出错了/牛牛走了"。
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
            }
            continue;
        }
        if (request.kind == BoardRequestKind::kStopCapture) {
            const Status stop =
                session_ ? session_->EndCapture() : Status::Error(ErrorCode::kUnavailable, "语音会话尚未启动");
            if (!stop.ok()) {
                ESP_LOGW(kTag, "板级按键结束采集失败: %s", stop.message.c_str());
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kFailure);
            } else {
                // 仅当已离开 kFinalizing（VAD 端点后等待最终 STT 中）才恢复待机：
                // kFinalizing 表示本轮还在等最终 STT/TTS，不能提前回待机。
                // 其余（聆听正常结束、超时、按键停止）恢复待机。
                if (interaction_orchestrator_.state() != voice::VoiceInteractionState::kFinalizing) {
                    QueueStandbyRecovery();
                }
            }
            continue;
        }
        if (request.kind == BoardRequestKind::kInterruptAndStartCapture) {
            if (!session_) continue;
            const Status interrupt = session_->Interrupt();
            const Status capture = interrupt.ok() ? session_->BeginCapture() : interrupt;
            if (!capture.ok()) {
                ESP_LOGW(kTag, "板级打断后开始采集失败: %s", capture.message.c_str());
                // 打断后启动失败：回待机，不显示"出错了/牛牛走了"。
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
            }
            continue;
        }
        if (!session_ || !provider_) continue;
        // Linx 官方协议支持 listen.detect.text_response：服务端真实合成
        // “收到！”并下发协商 PCM，tts.stop 后 Controller 才开始聆听。
        const Status acknowledge = session_->NotifyLocalWakeWord(request.wake_word, "收到！");
        if (!acknowledge.ok()) {
            ESP_LOGW(kTag, "唤醒确认请求失败: %s", acknowledge.message.c_str());
            // 唤醒启动失败：回待机，不显示"出错了/牛牛走了"。
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
        }
    }
}

// 显示模型：由会话阶段推导可见状态，仅在 revision 变化时提交渲染器。
}  // namespace voicelife::runtime
#endif
