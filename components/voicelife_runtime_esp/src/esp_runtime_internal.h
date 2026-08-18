#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "voicelife/application/interaction_orchestrator.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/esp_http_transport_factory.h"
#include "voicelife/im/im_binding_use_case.h"
#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/linx/linx_speech_provider.h"
#include "voicelife/linx/linx_types.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/runtime/platform_assembly.h"
#include "voicelife/runtime_esp/esp_interaction_task_host.h"
#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/voice/display_snapshot.h"
#include "voicelife/voice/voice_interaction_controller.h"
#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "bootstrap/storage_bootstrap.h"
#include "im_binding_polling_lease.h"
#include "im_binding_presentation.h"
#include "im_runtime_bootstrap.h"

namespace voicelife::runtime {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";
constexpr int64_t kWakeAckDisplayUs = 400 * 1000;
constexpr int64_t kVolumeOverlayUs = 1500 * 1000;
constexpr uint32_t kListenStartTimeoutMs = 6000;
constexpr uint32_t kFinalSttTimeoutMs = 5000;
#if CONFIG_VOICELIFE_IM_GATEWAY
constexpr bool kImGatewayEnabled = true;
#else
constexpr bool kImGatewayEnabled = false;
#endif

class NvsSecretResolver final : public linx_esp::SecretResolverPort {
   public:
    Result<std::string> Resolve(std::string_view reference) override;
};
#endif

class ScaffoldAudioInput final : public voice::AudioInputPort {
   public:
    void SetAudioSink(voice::AudioFrameSink) override {}
    Status Open(const voice::AudioFormat&) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    void Close() override {}
};

class ScaffoldAudioOutput final : public voice::AudioOutputPort {
   public:
    Status Open(const voice::AudioFormat&) override { return Status::Ok(); }
    Status Push(const voice::AudioFrame&) override { return Status::Ok(); }
    Status Flush() override { return Status::Ok(); }
    bool IsIdle() const override { return true; }
    void Close() override {}
};

class ScaffoldSpeechProvider final : public voice::SpeechProviderAdapter {
   public:
    Status Connect(const voice::VoiceSessionConfig&, voice::VoiceEventSink) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    Status SendAudio(const voice::AudioFrame&) override { return Status::Ok(); }
    Status Abort(std::string_view) override { return Status::Ok(); }
    Status Speak(std::string_view) override { return Status::Ok(); }
    Status NotifyLocalWakeWord(std::string_view, std::string_view = {}) override { return Status::Ok(); }
    Status Disconnect() override { return Status::Ok(); }
    Result<voice::VoiceAudioFormats> audio_formats() const override;
    const voice::CapabilityProfile& capabilities() const override { return profile_; }

   private:
    voice::CapabilityProfile profile_{"scaffold", {"streaming-asr", "tts"}};
};

class Runtime final : public application::InteractionActionSink {
   public:
    Runtime();
    Status Start(PlatformAssembly& assembly);
    Status RequestInterrupt();

   private:
    StorageBootstrap storage_;
#ifdef ESP_PLATFORM
    struct McpRequest {
        std::string payload;
        std::string session_id;
        std::mutex mutex;
        std::condition_variable completed_cv;
        std::optional<Result<std::string>> response;
        bool completed = false;
        std::atomic_bool abandoned{false};
    };
    enum class BoardRequestKind : uint8_t{
        kWakeWord,     kInterruptAndWakeWord, kRestoreStandby,           kInterrupt,
        kStartCapture, kStopCapture,          kInterruptAndStartCapture,
    };
    struct BoardRequest {
        BoardRequestKind kind = BoardRequestKind::kRestoreStandby;
        char wake_word[32];
        bool settle_controller = true;
        char system_speech[kBindingSystemSpeechCapacity];
    };
    struct InteractionEventItem {
        voice::VoiceInteractionEvent event = voice::VoiceInteractionEvent::kBootCompleted;
        std::string wake_word;
        std::string display_text;
        bool display_only = false;
        bool display_update = false;
        bool display_overlay = false;
        voice::VoiceMood display_mood = voice::VoiceMood::kIdle;
        std::string display_status;
        std::string display_content;
        bool voice_evidence = false;
        voice::VoiceEvidence evidence;
        bool binding_result = false;
        im::BindingResult binding;
        bool binding_reset = false;
        uint64_t binding_generation = 0;
        bool listen_timeout = false;
        bool network_update = false;
        bool network_connected = false;
        bool board_input = false;
        BoardInputAction board_action = BoardInputAction::kToggleChat;
    };

    void StopEventLoop();
    Status StartMcpWorker();
    void StopMcpWorker();
    void StartBindingPolling(uint64_t generation);
    static void BindingPollTaskEntry(void* context);
    void BindingPollLoop();
    static std::string TruncateUtf8(std::string_view value, std::size_t max_bytes);
    static bool IsMcpToolCall(std::string_view payload);
    Result<std::string> HandleMcpRequest(std::string_view payload, std::string_view session_id);
    static void McpWorkerTaskEntry(void* arg);
    void McpWorkerLoop();
    void StartImRuntime();
    static void ImLifecycleTaskEntry(void* context);
    void ImLifecycleTask();
    void EnqueueBoardInput(BoardInputAction action);
    void SetVolume(int volume);
    void QueueWakeWord(std::string_view wake_word);
    void QueueVoiceTurn(std::string_view wake_word);
    void QueueInterruptAndVoiceTurn(std::string_view wake_word);
    void QueueStandbyRecovery(bool settle_controller = true);
    bool QueueSystemSpeech(std::string_view text);
    static void VolumeOverlayEntry(void* context);
    static void ListenTimeoutEntry(void* context);
    void StartListenTimer(uint32_t timeout_ms);
    void CancelListenTimer();
    void QueueInterrupt();
    void QueueCaptureStart();
    void QueueCaptureStop();
    void QueueInterruptAndCapture();
#if CONFIG_VOICELIFE_STATE_FLOW_TEST
    Status StartStateFlowDiagnostic();
    static void StateFlowTaskEntry(void* context);
    void StateFlowEvent(uint32_t step, voice::VoiceInteractionEvent event);
    void StateFlowEvidence(uint32_t step, std::string_view event, std::string_view detail = {});
    void StateFlowTask();
#endif
    void RestoreStandby(const BoardRequest& request);
    static void WakeTaskEntry(void* context);
    void WakeTask();
    static std::string_view PhaseStatusText(voice::VoiceInteractionState state);
    static voice::VoiceMood PhaseMood(voice::VoiceInteractionState state);
    static std::string CurrentStandbyStatusText();
    void CommitSnapshot();
    void ShowDisplay(voice::VoiceMood mood, std::string_view status, std::string_view content);
    void ShowOverlay(voice::VoiceMood mood, std::string_view status, std::string_view content);
    void StartOverlayTimer(uint32_t duration_ms);
    void ClearExpiredWakeAck();
    Status HandleInteractionEvent(voice::VoiceInteractionEvent event, std::string_view wake_word = {});
    Status Submit(application::InteractionAction transition) override;
    void LogVoiceEvidence(const voice::VoiceEvidence& evidence);
    void ProcessVoiceEvidence(const voice::VoiceEvidence& evidence);
    void EnqueueEvent(voice::VoiceInteractionEvent event, std::string_view wake_word = {});
    void EnqueueDisplayText(std::string detail);
    void EnqueueDisplayUpdate(voice::VoiceMood mood, std::string_view status, std::string_view content, bool overlay);
    void EnqueueBindingResult(const im::BindingResult& result);
    void EnqueueBindingReset(uint64_t generation);
    void CancelBindingTerminalDisplay();
    void ClearExpiredBindingTerminalDisplay();
    void CommitBindingPresentation(const BindingPresentation& presentation);
    void QueueDeferredBindingSpeechIfStandby();
    void ProcessBindingResult(const im::BindingResult& result);
    void EnqueueVoiceEvidence(const voice::VoiceEvidence& evidence);
    void EnqueueListenTimeout();
    void EnqueueNetworkState(bool connected);
    static void EventLoopTaskEntry(void* arg);
    void EventLoopLoop();

    static constexpr std::size_t kMcpWorkerQueueCapacity = 4;
    static constexpr uint32_t kBindingPollIntervalMs = 3000;
    static constexpr uint32_t kBindingPollStackBytes = 16384;
    static constexpr std::size_t kEventQueueCapacity = 16;
    NvsSecretResolver linx_secrets_;
    NvsImSecretStore im_secret_store_;
    im::StoredImConfigProvider im_config_{im_secret_store_, kImGatewayEnabled};
    EspImRuntimeReadiness im_readiness_;
    im::ImRuntime im_runtime_{im_config_, im_config_, im_readiness_,
                              [](const std::string& origin) { return im::CreateEspHttpTransport(origin); }};
    EspPairingClock im_pairing_clock_;
    im::BindingUseCase binding_use_case_;
    BindingPollingLease binding_poll_lease_;
    bool binding_display_active_ = false;
    uint64_t binding_display_generation_ = 0;
    std::string binding_status_text_;
    std::string binding_content_text_;
    std::optional<BindingPresentation> deferred_binding_presentation_;
    std::string deferred_binding_speech_;
    std::atomic_bool im_lifecycle_started_{false};
    TaskHandle_t im_lifecycle_task_ = nullptr;
    mcp::McpServer mcp_server_;
    schedule::ScheduleService schedule_service_;
    schedule::ScheduleOperationService schedule_operation_service_;
    schedule::ScheduleRuleService schedule_rule_service_;
    Status init_status_ = Status::Ok();
    linx::LinxJsonCodec linx_codec_;
    linx::LinxConnectionConfig linx_config_;
    std::unique_ptr<linx_esp::EspWebSocketTransport> linx_transport_ =
        std::make_unique<linx_esp::EspWebSocketTransport>(linx_secrets_);
    QueueHandle_t wake_queue_ = nullptr;
    TaskHandle_t wake_task_ = nullptr;
#if CONFIG_VOICELIFE_STATE_FLOW_TEST
    TaskHandle_t state_flow_task_ = nullptr;
#endif
    std::deque<InteractionEventItem> event_queue_;
    mutable std::mutex event_mutex_;
    std::condition_variable event_cv_;
    TaskHandle_t event_task_ = nullptr;
    bool event_loop_stop_ = false;
    bool event_loop_stopped_ = false;
    std::atomic<bool> overlay_expired_{false};
    std::mutex mcp_mutex_;
    std::condition_variable mcp_cv_;
    std::deque<std::shared_ptr<McpRequest>> mcp_queue_;
    TaskHandle_t mcp_task_ = nullptr;
    bool mcp_stop_ = false;
    std::atomic_bool mcp_stopped_{true};
    int volume_ = 70;
    std::atomic<int64_t> capture_started_us_{0};
    std::string stt_display_text_;
    bool terminal_turn_ = false;
    bool binding_turn_awaiting_tts_completion_ = false;
    bool binding_terminal_display_active_ = false;
    bool binding_terminal_resume_listening_ = false;
    voice::VoiceMood binding_terminal_mood_ = voice::VoiceMood::kNeutral;
    std::string binding_terminal_status_text_;
    std::string binding_terminal_content_text_;
    int64_t binding_terminal_until_us_ = 0;
    std::string last_wake_word_;
    int64_t last_wake_at_ = 0;
    int64_t wake_ack_requested_at_us_ = 0;
    int64_t wake_ack_tts_started_at_us_ = 0;
    int64_t wake_ack_until_us_ = 0;
    int64_t volume_overlay_until_us_ = 0;
    esp_timer_handle_t volume_overlay_timer_ = nullptr;
    voice::DisplaySnapshot snapshot_;
    voice::DisplaySnapshot overlay_base_snapshot_;
    bool overlay_active_ = false;
    uint64_t last_rendered_revision_ = 0;
    PlatformAssembly* assembly_ = nullptr;
    esp_timer_handle_t listen_timer_ = nullptr;
#else
    ScaffoldAudioInput audio_input_;
    ScaffoldAudioOutput audio_output_;
#endif
    application::InteractionOrchestrator interaction_orchestrator_;
    runtime_esp::EspInteractionTaskHost interaction_task_host_{interaction_orchestrator_};
    std::unique_ptr<voice::SpeechProviderAdapter> provider_;
    std::unique_ptr<voice::VoiceSession> session_;
};

Runtime& Instance();

}  // namespace voicelife::runtime
