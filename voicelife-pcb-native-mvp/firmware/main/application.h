#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "ota.h"
#include "protocol.h"
#if CONFIG_VOICELIFE_ENABLED
#include "voicelife/voicelife_service.h"
#if CONFIG_VOICELIFE_IM_ENABLED
#include "voicelife/voicelife_im_http.h"
#include "voicelife/voicelife_im_sync.h"
#endif
#endif

// Main event bits
#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT (1 << 9)
#define MAIN_EVENT_START_LISTENING (1 << 10)
#define MAIN_EVENT_STOP_LISTENING (1 << 11)
#define MAIN_EVENT_STATE_CHANGED (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED (1 << 13)
#define MAIN_EVENT_VOICELIFE_AUDIO_QUIET (1 << 14)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "",
               const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
#if CONFIG_VOICELIFE_ENABLED
    bool SendTextMessage(const std::string& text);
#endif
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
#if CONFIG_VOICELIFE_ENABLED
    esp_timer_handle_t voicelife_audio_quiet_timer_handle_ = nullptr;
#endif
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;
#if CONFIG_VOICELIFE_ENABLED
    std::unique_ptr<voicelife::VoiceLifeService> voicelife_service_;
    // True while a local reminder is being delivered through Linx TTS.
    bool voicelife_announcement_active_ = false;
    bool voicelife_announcement_close_pending_ = false;
    bool voicelife_announcement_audio_seen_ = false;
    // Non-business final responses close after playback drains. Business
    // receipts and query answers open a bounded follow-up window.
    bool voicelife_single_turn_close_pending_ = false;
    bool voicelife_followup_listen_pending_ = false;
    // A wake acknowledgement always continues into command capture even
    // though its fixed response, "收到！", is not a question.
    bool voicelife_wake_ack_pending_ = false;
    std::atomic<bool> voicelife_response_needs_followup_{false};
    std::atomic<bool> voicelife_business_response_completed_{false};
    std::atomic<bool> voicelife_business_tools_allowed_{false};
    // MCP results share the WebSocket with microphone audio. Stop producing
    // audio while Linx processes a tool result so the control frame cannot be
    // starved behind a continuously growing audio queue.
    bool voicelife_mcp_waiting_for_tts_ = false;
    // A VoiceLife command is a bounded capture turn. Linx normally performs
    // endpoint detection, while these fields provide a local fallback and
    // prevent ambient speech from leaking into the turn after final STT.
    bool voicelife_capture_stop_sent_ = false;
    std::atomic<bool> voicelife_stt_received_{false};
    std::atomic<bool> voicelife_capture_speech_seen_{false};
    std::atomic<bool> voicelife_capture_vad_speaking_{false};
    std::atomic<int64_t> voicelife_capture_started_us_{0};
    std::atomic<int64_t> voicelife_capture_first_speech_us_{0};
    std::atomic<int64_t> voicelife_capture_silence_started_us_{0};
    std::atomic<int64_t> voicelife_conversation_deadline_us_{0};
    // Updated by the network callback and read by the main task. The clock
    // tick provides a fallback when a transport omits tts.stop or the audio
    // decoder never reports an empty queue.
    std::atomic<int64_t> voicelife_last_audio_time_us_{0};
#if CONFIG_VOICELIFE_IM_ENABLED
    std::unique_ptr<voicelife::VoiceLifeImHttpTransport> voicelife_im_transport_;
    std::unique_ptr<voicelife::VoiceLifeImSync> voicelife_im_sync_;
    TaskHandle_t voicelife_im_task_handle_ = nullptr;
    std::atomic<bool> voicelife_im_stop_{false};
    bool voicelife_im_started_ = false;
#endif
#endif

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ =
        false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ =
        false;  // Waiting for playback to drain before starting listening (auto mode)
    bool defer_listening_for_playback_ = false;  // Set only when TTS continues into auto listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;

    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void MaybeFinishVoiceLifeAnnouncement();
    void MaybeStopVoiceLifeCapture();
    void StopVoiceLifeCapture(const char* reason, bool notify_server);
    void ResetVoiceLifeCapture();
    void MaybeCloseVoiceLifeConversation();
#if CONFIG_VOICELIFE_IM_ENABLED
    void StartVoiceLifeImSync();
    void VoiceLifeImTask();
#endif
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};

class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() { vTaskPrioritySet(NULL, original_priority_); }

private:
    BaseType_t original_priority_;
};

#endif  // _APPLICATION_H_
