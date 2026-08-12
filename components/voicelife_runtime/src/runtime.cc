#include "voicelife/runtime/runtime.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#ifdef ESP_PLATFORM
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "generated/farewell_pcm.h"
#include "generated/wake_ack_pcm.h"
#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#include "voicelife/linx/linx_speech_provider.h"
#include "voicelife/linx/linx_types.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"
#endif

#include "linx_mcp_bridge.h"
#include "linx_ota_bootstrap.h"
#include "linx_secret_resolver.h"
#include "runtime_audio_diagnostics.h"
#include "runtime_board_input.h"
#include "runtime_presentation.h"
#include "runtime_scaffold.h"
#include "runtime_voice_wiring.h"
#include "schedule_mcp_tools.h"
#include "voicelife/voice/voice_interaction_controller.h"
#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";
constexpr uint32_t kWakeFeedbackMs = 800;
constexpr int64_t kWakeAckDisplayUs = 400 * 1000;
constexpr uint32_t kListenTimeoutMs = 15000;
constexpr uint32_t kFinalSttTimeoutMs = 5000;

#endif

class Runtime final {
   public:
    Runtime() {
        auto& registry = voice::SpeechProviderRegistry::Instance();
#ifdef ESP_PLATFORM
        init_status_ = RegisterScheduleMcpTools(mcp_server_, schedule_service_);
        if (init_status_.ok()) {
            ESP_LOGI(kTag, "MCP_TOOLS_READY count=2 names=schedule.create,schedule.query");
        }
        registry.Register("xrobot-websocket", linx::LinxSpeechProviderAdapter::DefaultCapabilities(), [this]() {
            return std::make_unique<linx::LinxSpeechProviderAdapter>(
                *linx_transport_, linx_codec_, linx_config_, linx::LinxSpeechProviderAdapter::DefaultCapabilities(),
                [this](std::string_view payload, std::string_view session_id) {
                    return HandleLinxMcpPayload(payload, mcp_server_, session_id);
                });
        });
#endif
        registry.Register("scaffold", voice::CapabilityProfile{"scaffold", {"streaming-asr", "tts"}},
                          []() { return std::make_unique<ScaffoldSpeechProvider>(); });
    }

    Status Start() {
        auto& registry = voice::SpeechProviderRegistry::Instance();
        if (!init_status_.ok()) return init_status_;
#ifdef ESP_PLATFORM
        presentation_.InitializeHardware();
        presentation_.ShowNetworkSetup();
        if (const Status secret_store = InitializeLinxSecretStore(); !secret_store.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=secret_store code=%d", static_cast<int>(secret_store.code));
            presentation_.ShowStartupError();
            return secret_store;
        }
        auto connection = BootstrapLinxOtaConfig();
        if (!connection.ok() || !connection.value.has_value()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=linx_bootstrap code=%d", static_cast<int>(connection.status.code));
            presentation_.ShowStartupError();
            return connection.status;
        }
        presentation_.ShowServiceConnecting();
        linx_config_ = std::move(*connection.value);
        auto result = registry.Create("xrobot-websocket", {});
#else
        auto result = registry.Create("scaffold", {});
#endif
        if (!result.ok() || !result.value.has_value()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=provider_create code=%d", static_cast<int>(result.status.code));
            return Status::Error(ErrorCode::kInternal, "无法创建语音 Provider: " + result.status.message);
        }
        provider_ = std::move(*result.value);

#ifdef ESP_PLATFORM
        voice_wiring_.Assemble(
            *provider_, [this](const voice::VoiceEvidence& evidence) { LogVoiceEvidence(evidence); },
            [this](std::string_view wake_word) { QueueWakeWord(wake_word); }, volume_);
        session_ = voice_wiring_.session();
        const Status session_status = voice_wiring_.StartSession();
        if (!session_status.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=session_start code=%d", static_cast<int>(session_status.code));
            presentation_.ShowStartupError();
            return session_status;
        }
#else
        host_session_ = std::make_unique<voice::VoiceSession>(audio_input_, audio_output_, *provider_);
        voice::VoiceSessionConfig config;
        config.session_id = "scaffold-session";
        config.provider_id = "scaffold";
        session_ = host_session_.get();
        const Status session_status = host_session_->Start(config);
        if (!session_status.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=session_start code=%d", static_cast<int>(session_status.code));
            return session_status;
        }
#endif

#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_AUDIO_PROBE
        audio_esp::Esp32s3AudioProbe probe;
        const auto profile =
#if CONFIG_VOICELIFE_AUDIO_PROBE_PROFILE_VOICELIFE_PCB
            audio_esp::VoiceLifePcbEsp32s3Profile();
#else
            audio_esp::LichuangEsp32s3Profile();
#endif
        audio_esp::AudioProbeOptions options;
#if CONFIG_VOICELIFE_AUDIO_PROBE_REPLAY
        options.replay_capture = true;
#endif
        const auto probe_result = probe.Run(profile, options);
        if (!probe_result.ok() || !probe_result.value.has_value()) {
            return probe_result.status;
        }
        const auto& report = *probe_result.value;
        ESP_LOGI(kTag,
                 "音频探针：profile=%s codec_required=%d I2C=%d ES8311=%d ES7210=%d "
                 "PCA9557=%d I2S_READY=%d I2S_STARTED=%d bus_write=%u bus_read=%u "
                 "pcm_samples=%u nonzero=%u changed=%u saturated=%u saturation_ppm=%llu "
                 "peak=%u mean_square=%llu signal=%d replay=%u tone_written=%u tone_ok=%d min_heap=%u",
                 profile.id.c_str(), report.codec_control_required, report.i2c_bus_ready, report.es8311_ack,
                 report.es7210_ack, report.pca9557_ack, report.i2s_channels_ready, report.i2s_channels_started,
                 static_cast<unsigned>(report.bytes_written), static_cast<unsigned>(report.bytes_read),
                 static_cast<unsigned>(report.capture_samples), static_cast<unsigned>(report.nonzero_samples),
                 static_cast<unsigned>(report.changed_samples), static_cast<unsigned>(report.saturated_samples),
                 static_cast<unsigned long long>(report.saturation_ratio_ppm()), static_cast<unsigned>(report.peak_abs),
                 static_cast<unsigned long long>(report.mean_square()), report.capture_signal_detected(),
                 static_cast<unsigned>(report.replay_bytes_written), static_cast<unsigned>(report.probe_tone_written),
                 report.probe_tone_ok ? 1 : 0, static_cast<unsigned>(report.minimum_free_heap_bytes));
        if (!report.hardware_ready()) {
            return Status::Error(ErrorCode::kUnavailable, "音频探针硬件未就绪，Codec ACK 或 I2S 状态不完整");
        }
        if (!report.capture_signal_detected()) {
            return Status::Error(ErrorCode::kUnavailable, "音频探针未检测到可变化的 PCM 输入");
        }
#endif
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_AUDIO_PORT_SMOKE
        const Status audio_port_status = RunVoiceLifePcbAudioPortSmoke();
        if (!audio_port_status.ok()) {
            return audio_port_status;
        }
#endif
#ifdef ESP_PLATFORM
        if (wake_queue_ == nullptr) {
            wake_queue_ = xQueueCreate(4, sizeof(BoardRequest));
            if (wake_queue_ == nullptr) return Status::Error(ErrorCode::kInternal, "创建唤醒队列失败");
            const BaseType_t task_status =
                xTaskCreate(&Runtime::WakeTaskEntry, "voicelife_wake", 4096, this, 5, &wake_task_);
            if (task_status != pdPASS) return Status::Error(ErrorCode::kInternal, "创建唤醒控制任务失败");
        }
        const Status interaction_status = HandleInteractionEvent(voice::VoiceInteractionEvent::kBootCompleted);
        if (!interaction_status.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=interaction_boot code=%d", static_cast<int>(interaction_status.code));
            return interaction_status;
        }
        board_input_ = std::make_unique<VoiceLifePcbBoardInput>(
            [this](voice::VoiceInteractionEvent event) { (void)HandleInteractionEvent(event); },
            [this](int volume_delta_or_absolute) {
                if (volume_delta_or_absolute == 100 || volume_delta_or_absolute == 0) {
                    SetVolume(volume_delta_or_absolute);
                } else {
                    SetVolume(std::clamp(volume_ + volume_delta_or_absolute, 0, 100));
                }
            });
        board_input_->Start();
#endif
        return Status::Ok();
    }

   private:
#ifdef ESP_PLATFORM
    enum class BoardRequestKind : uint8_t {
        kWakeWord,
        kRestoreStandby,
        kInterrupt,
        kStartCapture,
        kStopCapture,
        kInterruptAndStartCapture,
    };

    struct BoardRequest {
        BoardRequestKind kind = BoardRequestKind::kRestoreStandby;
        char wake_word[32];
    };

    void SetVolume(int volume) {
        volume_ = std::clamp(volume, 0, 100);
        voice_wiring_.SetOutputVolume(volume_);
        presentation_.ShowVolume(volume_);
    }

    // 本地提示音：播放 popup.ogg 解码的 PCM（16kHz S16LE），按协商播放格式重采样。
    // 播放本地提示音（裸 PCM 16kHz mono）：直接入队，播放端口统一重采样到 24kHz。
    void PlayPrompt(const int16_t* pcm, size_t sample_count) {
        if (voice_wiring_.audio_ports() == nullptr || pcm == nullptr || sample_count == 0) return;
        voice::AudioFrame frame;
        frame.format = {.codec = voice::AudioCodec::kPcmS16Le,
                        .sample_rate_hz = 16000,
                        .channels = 1,
                        .bits_per_sample = 16,
                        .frame_duration_ms = 0};
        frame.generation = 0;
        frame.sequence = 0;
        frame.payload.resize(sample_count * sizeof(int16_t));
        std::memcpy(frame.payload.data(), pcm, sample_count * sizeof(int16_t));
        const Status push_status = voice_wiring_.audio_ports()->output().Push(frame);
        ESP_LOGI(kTag, "PROMPT_PUSH result=%d bytes=%u", push_status.ok() ? 1 : 0,
                 static_cast<unsigned>(frame.payload.size()));
    }

    // 唤醒“收到”提示音。
    void PlayWakeAck() { PlayPrompt(wake_ack::kPcm, wake_ack::kSampleCount); }

    // 告别“牛牛走了”提示音。
    void PlayFarewell() { PlayPrompt(farewell::kPcm, farewell::kSampleCount); }

    void QueueWakeWord(std::string_view wake_word) {
        LogVoiceEvidence({.session_id = session_ ? session_->config().session_id : "",
                          .generation = session_ ? session_->generation() : 0,
                          .event = "wake_detected",
                          .detail = {}});
        // 记录唤醒词与时间，用于抑制服务端把唤醒词回传为 STT。
        last_wake_word_ = wake_word;
        last_wake_at_ = esp_timer_get_time();
        // WakeAck 显示租约：开麦（kWakeDetected 动作）不延迟；租约期内
        // Presentation 显示“收到！”，到期后切换普通聆听。
        wake_ack_until_us_ = esp_timer_get_time() + kWakeAckDisplayUs;
        PlayWakeAck();
        const Status wake_status = HandleInteractionEvent(voice::VoiceInteractionEvent::kWakeDetected, wake_word);
        if (!wake_status.ok()) {
            // 唤醒被拒（如控制器不在 kStandby，可能是假待机残留）：MultiNet 已一次性
            // 停止，必须重启检测器，否则后续永远叫不醒。
            ESP_LOGW(kTag, "WAKE_REJECTED state=%d err=%s", static_cast<int>(interaction_.state()),
                     wake_status.message.c_str());
            if (auto* wake_gate = voice_wiring_.wake_gate()) {
                (void)wake_gate->StartStandby();
            }
        }
    }

    void QueueVoiceTurn(std::string_view wake_word) {
        if (wake_queue_ == nullptr) return;
        BoardRequest request{};
        request.kind = BoardRequestKind::kWakeWord;
        const std::size_t size =
            wake_word.size() < sizeof(request.wake_word) - 1 ? wake_word.size() : sizeof(request.wake_word) - 1;
        std::memcpy(request.wake_word, wake_word.data(), size);
        request.wake_word[size] = '\0';
        (void)xQueueSend(wake_queue_, &request, 0);
    }

    void QueueStandbyRecovery() {
        if (wake_queue_ == nullptr) return;
        const BoardRequest recovery{};
        (void)xQueueSend(wake_queue_, &recovery, 0);
    }

    // 聆听/最终 STT 超时：
    // - kListening 超时（无有效输入）：结束本轮回待机
    // - kFinalizing 超时（listen.stop 后 5s 无最终 STT）：abort 结束服务端回合回待机
    static void ListenTimeoutEntry(void* context) {
        auto* self = static_cast<Runtime*>(context);
        if (self->interaction_.state() == voice::VoiceInteractionState::kListening) {
            // 单一收尾路径：kPressUp → kStopVoiceTurn → EndCapture（发 listen.stop
            // 结束服务端回合并回待机）。不得先 Interrupt() 再 kPressUp：Interrupt
            // 已把 Session 置回 Ready，随后 EndCapture 会返回“当前没有采集”并进
            // Error，造成双重收尾竞态。
            self->HandleInteractionEvent(voice::VoiceInteractionEvent::kPressUp);
        } else if (self->interaction_.state() == voice::VoiceInteractionState::kFinalizing) {
            // 最终 STT 超时：先 abort 清理服务端残留回合，再走状态机
            // kFinalizationTimedOut（kFinalizing→kStandby）恢复待机。
            ESP_LOGI(kTag, "FINALIZE_TIMEOUT transition=finalizing->standby");
            if (self->session_) {
                (void)self->session_->Interrupt();
            }
            (void)self->HandleInteractionEvent(voice::VoiceInteractionEvent::kFinalizationTimedOut);
        }
    }

    void StartListenTimer(uint32_t timeout_ms) {
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
        (void)esp_timer_start_once(listen_timer_, timeout_ms * 1000ULL);
    }

    void CancelListenTimer() {
        if (listen_timer_ != nullptr) {
            (void)esp_timer_stop(listen_timer_);
        }
    }

    void QueueInterrupt() {
        if (wake_queue_ == nullptr) return;
        BoardRequest request{};
        request.kind = BoardRequestKind::kInterrupt;
        (void)xQueueSend(wake_queue_, &request, 0);
    }

    void QueueCaptureStart() {
        if (wake_queue_ == nullptr) return;
        BoardRequest request{};
        request.kind = BoardRequestKind::kStartCapture;
        (void)xQueueSend(wake_queue_, &request, 0);
    }

    void QueueCaptureStop() {
        if (wake_queue_ == nullptr) return;
        BoardRequest request{};
        request.kind = BoardRequestKind::kStopCapture;
        (void)xQueueSend(wake_queue_, &request, 0);
    }

    void QueueInterruptAndCapture() {
        if (wake_queue_ == nullptr) return;
        BoardRequest request{};
        request.kind = BoardRequestKind::kInterruptAndStartCapture;
        (void)xQueueSend(wake_queue_, &request, 0);
    }

    void RestoreStandby() {
        auto* wake_gate = voice_wiring_.wake_gate();
        if (wake_gate == nullptr) return;
        const Status stop_status = wake_gate->StopCapture();
        if (!stop_status.ok()) {
            ESP_LOGW(kTag, "本地待机恢复停止上行失败: %s", stop_status.message.c_str());
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFailure);
            return;
        }
        const Status standby_status = wake_gate->StartStandby();
        if (!standby_status.ok()) {
            ESP_LOGW(kTag, "本地待机恢复失败: %s", standby_status.message.c_str());
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFailure);
            return;
        }
        LogVoiceEvidence({.session_id = session_ ? session_->config().session_id : "",
                          .generation = session_ ? session_->generation() : 0,
                          .event = "standby_ready",
                          .detail = {}});
        // 会话结束反馈：先等播放排空（避免 TTS 还在播就显示“牛牛走了！”），
        // 仅非首次待机显示反馈，短暂停留后回到空闲。
        if (auto* audio_ports = voice_wiring_.audio_ports()) {
            for (int i = 0; i < 30 && !audio_ports->output().IsIdle(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (first_standby_) {
            first_standby_ = false;
        } else {
            presentation_.ShowFarewell();
            PlayFarewell();
            vTaskDelay(pdMS_TO_TICKS(kWakeFeedbackMs));
        }
        // 显式派发 kStandbyReady：Controller 从 Error/kFinalizing 回 Standby，
        // 避免 RestoreStandby 直接写快照造成控制器仍停 Error 的假待机
        // （WAKE_REARM atomic=0）。Controller 回 Standby 后由状态机动作
        // 统一提交时间快照。
        const Status ready_status = HandleInteractionEvent(voice::VoiceInteractionEvent::kStandbyReady);
        if (!ready_status.ok()) {
            ESP_LOGW(kTag, "STAND_BY_READY_REJECTED state=%d err=%s", static_cast<int>(interaction_.state()),
                     ready_status.message.c_str());
        }
        // 待机原子条件校验：控制器/会话/唤醒门三态一致。
        // 仅全部满足才显示 Standby 时间快照；不满足则为假待机，保留告警文案并记录。
        const bool controller_ok = interaction_.state() == voice::VoiceInteractionState::kStandby;
        const bool session_ok = session_ && session_->state() == voice::VoiceSessionState::kReady;
        const bool gate_ok = wake_gate && wake_gate->standby();
        const bool atomic_ok = controller_ok && session_ok && gate_ok;
        ESP_LOGI(kTag, "WAKE_REARM controller=%d session=%d gate=%d atomic=%d", controller_ok ? 1 : 0,
                 session_ok ? 1 : 0, gate_ok ? 1 : 0, atomic_ok ? 1 : 0);
        if (atomic_ok) {
            presentation_.ShowStandby();
        }
        ESP_LOGI(kTag, "WAKE_STANDBY_READY=%d", atomic_ok ? 1 : 0);
    }

    static void WakeTaskEntry(void* context) { static_cast<Runtime*>(context)->WakeTask(); }

    void WakeTask() {
        BoardRequest request{};
        while (true) {
            if (xQueueReceive(wake_queue_, &request, portMAX_DELAY) != pdTRUE) continue;
            if (request.kind == BoardRequestKind::kRestoreStandby) {
                RestoreStandby();
                continue;
            }
            if (request.kind == BoardRequestKind::kInterrupt) {
                if (!session_) continue;
                const Status interrupt = session_->Interrupt();
                if (interrupt.ok()) {
                    if (interaction_.state() == voice::VoiceInteractionState::kInterrupting) {
                        (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kInterruptCompleted);
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
                if (auto* audio_ports = voice_wiring_.audio_ports()) {
                    for (int i = 0; i < 30 && !audio_ports->output().IsIdle(); ++i) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                const Status capture =
                    session_ ? session_->BeginCapture() : Status::Error(ErrorCode::kUnavailable, "语音会话尚未启动");
                if (!capture.ok()) {
                    ESP_LOGW(kTag, "板级按键开始采集失败: %s", capture.message.c_str());
                    // 事务式启动失败：回待机（kStandbyReady），不显示"出错了/牛牛走了"。
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kStandbyReady);
                }
                continue;
            }
            if (request.kind == BoardRequestKind::kStopCapture) {
                const Status stop =
                    session_ ? session_->EndCapture() : Status::Error(ErrorCode::kUnavailable, "语音会话尚未启动");
                if (!stop.ok()) {
                    ESP_LOGW(kTag, "板级按键结束采集失败: %s", stop.message.c_str());
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFailure);
                } else {
                    // 仅当已离开 kFinalizing（VAD 端点后等待最终 STT 中）才恢复待机：
                    // kFinalizing 表示本轮还在等最终 STT/TTS，不能提前回待机。
                    // 其余（聆听正常结束、超时、按键停止）恢复待机。
                    if (interaction_.state() != voice::VoiceInteractionState::kFinalizing) {
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
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kStandbyReady);
                }
                continue;
            }
            if (!session_ || !provider_) continue;
            // 本板不发送唤醒音频，故不发 listen.detect（否则服务端会把唤醒词
            // 当 STT 转写并生成一条问候回复）。对齐小智：直接 listen.start
            // 进入聆听，服务端只会把 start 之后的用户语音当输入。
            const Status capture = session_->BeginCapture();
            if (!capture.ok()) {
                ESP_LOGW(kTag, "唤醒后开始采集失败: %s", capture.message.c_str());
                // 唤醒启动失败：回待机，不显示"出错了/牛牛走了"。
                (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kStandbyReady);
            }
        }
    }

    Status HandleInteractionEvent(voice::VoiceInteractionEvent event, std::string_view wake_word = {}) {
        const auto transition = interaction_.Handle(event);
        if (!transition.ok() || !transition.value.has_value()) {
            ESP_LOGW(kTag, "忽略乱序板端交互事件=%d: %s", static_cast<int>(event), transition.status.message.c_str());
            return transition.status;
        }
        const bool show_wake_ack = wake_ack_until_us_ > 0 && esp_timer_get_time() < wake_ack_until_us_;
        if (event == voice::VoiceInteractionEvent::kEndpointDetected) wake_ack_until_us_ = 0;
        presentation_.ApplyInteraction(interaction_.state(), event, show_wake_ack, stt_display_text_);
        switch (transition.value->action) {
            case voice::VoiceInteractionAction::kNone:
                return Status::Ok();
            case voice::VoiceInteractionAction::kStartCapture:
                QueueCaptureStart();
                return Status::Ok();
            case voice::VoiceInteractionAction::kStartVoiceTurn:
                if (wake_word.empty()) {
                    return Status::Error(ErrorCode::kInvalidArgument, "本地唤醒词不能为空");
                }
                QueueVoiceTurn(wake_word);
                return Status::Ok();
            case voice::VoiceInteractionAction::kStopVoiceTurn:
                QueueCaptureStop();
                return Status::Ok();
            case voice::VoiceInteractionAction::kInterruptAndStartCapture:
                QueueInterruptAndCapture();
                return Status::Ok();
            case voice::VoiceInteractionAction::kRestoreStandby:
                QueueStandbyRecovery();
                return Status::Ok();
            case voice::VoiceInteractionAction::kInterruptSession:
                QueueInterrupt();
                return Status::Ok();
        }
        return Status::Error(ErrorCode::kInternal, "未知板端交互动作");
    }

    void LogVoiceEvidence(const voice::VoiceEvidence& evidence) {
        // Evidence detail can contain STT text or service diagnostics. Emit
        // only lifecycle names and numeric counters needed for board review.
        if (evidence.event == "capture_started") {
            capture_started_us_.store(esp_timer_get_time());
            StartListenTimer(kListenTimeoutMs);
        }
        const int64_t started_at = capture_started_us_.load();
        const int64_t now = esp_timer_get_time();
        const uint64_t latency_ms =
            started_at > 0 && now >= started_at ? static_cast<uint64_t>((now - started_at) / 1000) : 0;
        const auto stats = voice_wiring_.audio_stats();
        ESP_LOGI(kTag,
                 "VOICE_EVENT session=%s generation=%llu event=%s detail_present=%d latency_from_capture_ms=%llu "
                 "audio_captured=%u audio_dropped=%u audio_played=%u audio_rejected=%u min_heap=%u",
                 evidence.session_id.c_str(), static_cast<unsigned long long>(evidence.generation),
                 evidence.event.c_str(), evidence.detail.empty() ? 0 : 1, static_cast<unsigned long long>(latency_ms),
                 static_cast<unsigned>(stats.captured_frames), static_cast<unsigned>(stats.dropped_input_frames),
                 static_cast<unsigned>(stats.played_frames), static_cast<unsigned>(stats.rejected_output_frames),
                 static_cast<unsigned>(stats.minimum_free_heap_bytes));
        if (evidence.event == "provider_error") {
            // 板端诊断：只输出本地错误消息（不包含 STT 文本、凭据或原始响应）。
            ESP_LOGW(kTag, "PROVIDER_ERROR_DETAIL=%.160s", evidence.detail.c_str());
        }
        if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted" || evidence.event == "provider_error" ||
            evidence.event == "capture_stop_failed" || evidence.event == "tts_capture_stop_failed") {
            capture_started_us_.store(0);
        }
        if (evidence.event == "capture_started") {
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kCaptureStarted);
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
                terminal_turn_ = (evidence.detail.find("再见") != std::string::npos ||
                                  evidence.detail.find("拜拜") != std::string::npos ||
                                  evidence.detail.find("bye") != std::string::npos ||
                                  evidence.detail.find("拜") != std::string::npos ||
                                  evidence.detail.find("走了") != std::string::npos);
            }
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kIntentReceived);
        } else if (evidence.event == "tool_call_received") {
            // MCP 工具调用（服务端发现/工具执行）不是用户语音意图：
            // 仅取消聆听超时，不武装回复、不触发 kIntentReceived。
            CancelListenTimer();
        } else if (evidence.event == "tts_started") {
            CancelListenTimer();
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kTtsStarted);
        } else if (evidence.event == "tts_sentence_started") {
            // 回写服务端回复句子到屏幕（detail 为 TTS 文本），并立即提交快照
            // 让“说话中 + 助手文本”可见（不再停留显示用户 STT）。
            // 门控：仅当 Controller 已接受 kTtsStarted（处于 kSpeaking）才改显示；
            // 迟到的 TTS（Controller 已回 Standby/Error）直接丢弃，避免绕过状态机
            // 把屏幕卡在“说话中”。
            if (interaction_.state() != voice::VoiceInteractionState::kSpeaking) {
                ESP_LOGI(kTag, "TTS_SENTENCE_STALE state=%d 丢弃迟到句子", static_cast<int>(interaction_.state()));
                return;
            }
            CancelListenTimer();
            if (!evidence.detail.empty()) {
                stt_display_text_ = evidence.detail;
                presentation_.ShowAssistantText(evidence.detail);
            }
        } else if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted") {
            CancelListenTimer();
            presentation_.StopScroll();
            if (terminal_turn_) {
                // 终止回合（再见/拜拜）：告别播报完成走状态机 kFarewellCompleted
                // （kSpeaking→kStandby）恢复待机，不直接 QueueStandbyRecovery。
                terminal_turn_ = false;
                (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFarewellCompleted);
            } else {
                const Status stop_status = HandleInteractionEvent(voice::VoiceInteractionEvent::kTtsStopped);
                if (!stop_status.ok()) {
                    // Controller 已不在 kSpeaking（如迟到 TTS 触发时已回 Standby/
                    // Error）：强制清内容栏，避免屏幕卡在“说话中”。
                    ESP_LOGI(kTag, "TTS_STOPPED_STALE state=%d 强制回内容栏", static_cast<int>(interaction_.state()));
                    presentation_.ClearContent();
                }
            }
        } else if (evidence.event == "transport_disconnected") {
            CancelListenTimer();
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kTransportDisconnected);
        } else if (evidence.event == "transport_connected") {
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kTransportConnected);
        } else if (evidence.event == "provider_error" || evidence.event == "capture_stop_failed" ||
                   evidence.event == "tts_capture_stop_failed") {
            CancelListenTimer();
            // 会话已回待机后收到的 provider_error（如服务端有序 FIN/断开）是
            // 正常断线，不当作故障；随后的 transport_disconnected 走自动重连。
            // 仅会话进行中（聆听/处理/播报）的 provider_error 才算真正故障。
            const auto phase = interaction_.state();
            if (phase != voice::VoiceInteractionState::kStandby) {
                (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFailure);
            }
        } else if (evidence.event == "capture_stopped") {
            // kFinalizing（等最终 STT）时不得取消 5s 最终 STT 定时器，
            // 否则服务端不返回 STT 时会永久悬挂；其余状态取消。
            if (interaction_.state() != voice::VoiceInteractionState::kFinalizing) {
                CancelListenTimer();
            }
        } else if (evidence.event == "vad_silence") {
            // 本地 VAD 端点：用户说完话后静音 1200ms，发 listen.stop 使服务端
            // 进入最终 STT，然后等待最终 STT（kFinalizing），不回待机。
            // 启动 5s 最终 STT 超时：无 STT 则 abort 收尾。
            CancelListenTimer();
            if (interaction_.state() == voice::VoiceInteractionState::kListening) {
                (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kEndpointDetected);
                StartListenTimer(kFinalSttTimeoutMs);
            }
        }
    }

    NvsSecretResolver linx_secrets_;
    mcp::McpServer mcp_server_;
    schedule::ScheduleService schedule_service_;
    Status init_status_ = Status::Ok();
    linx::LinxJsonCodec linx_codec_;
    linx::LinxConnectionConfig linx_config_;
    std::unique_ptr<linx_esp::EspWebSocketTransport> linx_transport_ =
        std::make_unique<linx_esp::EspWebSocketTransport>(linx_secrets_);
    VoiceLifePcbVoiceWiring voice_wiring_;
    QueueHandle_t wake_queue_ = nullptr;
    TaskHandle_t wake_task_ = nullptr;
    std::unique_ptr<VoiceLifePcbBoardInput> board_input_;
    int volume_ = 70;
    std::atomic<int64_t> capture_started_us_{0};
    bool first_standby_ = true;
    std::string stt_display_text_;
    VoiceLifePcbPresentation presentation_;
    // 本轮是否为终止回合（用户说“再见/拜拜”等）：播报结束后不进入 follow-up。
    bool terminal_turn_ = false;
    // 最近唤醒词与其发生时刻（抑制唤醒词被服务端回传为 STT）。
    std::string last_wake_word_;
    int64_t last_wake_at_ = 0;
    // WakeAck 显示租约截止时刻（esp_timer_us）：到期前下行栏显示“收到！”。
    int64_t wake_ack_until_us_ = 0;
    esp_timer_handle_t listen_timer_ = nullptr;
#else
    ScaffoldAudioInput audio_input_;
    ScaffoldAudioOutput audio_output_;
#endif
    voice::VoiceInteractionController interaction_;
    std::unique_ptr<voice::SpeechProviderAdapter> provider_;
#ifndef ESP_PLATFORM
    std::unique_ptr<voice::VoiceSession> host_session_;
#endif
    voice::VoiceSession* session_ = nullptr;

   public:
    Status RequestInterrupt() {
        if (!session_) return Status::Error(ErrorCode::kUnavailable, "设备运行时尚未启动");
#ifdef ESP_PLATFORM
        return HandleInteractionEvent(voice::VoiceInteractionEvent::kInterruptRequested);
#else
        return Status::Error(ErrorCode::kUnavailable, "板端打断仅支持 ESP 平台");
#endif
    }
};

}  // namespace

Runtime& Instance() {
    static Runtime runtime;
    return runtime;
}

Status Start() { return Instance().Start(); }

Status RequestInterrupt() { return Instance().RequestInterrupt(); }

}  // namespace voicelife::runtime
