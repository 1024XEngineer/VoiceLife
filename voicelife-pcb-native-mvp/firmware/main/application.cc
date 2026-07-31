#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"
#if CONFIG_VOICELIFE_ENABLED
#include "voicelife/voicelife_mcp.h"
#include "voicelife/voicelife_turn_policy.h"
#if CONFIG_VOICELIFE_IM_ENABLED
#include "voicelife/voicelife_im_http.h"
#endif
#endif

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>

#define TAG "Application"

namespace {
constexpr size_t kMaxAudioPacketsPerMainLoop = 4;
}

#if CONFIG_VOICELIFE_ENABLED
constexpr int64_t kVoiceLifeAudioQuietUs = 2 * 1000 * 1000;
constexpr int64_t kVoiceLifeAudioHardTimeoutUs = 15 * 1000 * 1000;
constexpr int64_t kVoiceLifeFollowupTimeoutUs = 15 * 1000 * 1000;
constexpr int64_t kVoiceLifeResponseTimeoutUs = 30 * 1000 * 1000;
constexpr int64_t kVoiceLifeSttFinalizeTimeoutUs = 5 * 1000 * 1000;
constexpr int64_t kVoiceLifeMcpActivityTimeoutUs = 60 * 1000 * 1000;
constexpr int64_t kVoiceLifeMcpResultTimeoutUs = 12 * 1000 * 1000;
constexpr int64_t kVoiceLifeEndpointSilenceUs = 900 * 1000;
constexpr int64_t kVoiceLifeCaptureMaxUs = 6 * 1000 * 1000;
constexpr int64_t kVoiceLifeNoSpeechTimeoutUs = kVoiceLifeFollowupTimeoutUs;
constexpr char kVoiceLifeProcessingStatus[] = "处理中...";
#endif

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

#if CONFIG_VOICELIFE_ENABLED
    esp_timer_create_args_t voicelife_quiet_timer_args = {
        .callback =
            [](void* arg) {
                auto* app = static_cast<Application*>(arg);
                xEventGroupSetBits(app->event_group_, MAIN_EVENT_VOICELIFE_AUDIO_QUIET);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "voicelife_tts_quiet",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&voicelife_quiet_timer_args, &voicelife_audio_quiet_timer_handle_);
#endif
}

Application::~Application() {
#if CONFIG_VOICELIFE_ENABLED
    if (voicelife_audio_quiet_timer_handle_ != nullptr) {
        esp_timer_stop(voicelife_audio_quiet_timer_handle_);
        esp_timer_delete(voicelife_audio_quiet_timer_handle_);
    }
#if CONFIG_VOICELIFE_IM_ENABLED
    voicelife_im_stop_.store(true);
#endif
#endif
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        const EventBits_t bits = xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
        static bool first_audio_event_posted = false;
        if (!first_audio_event_posted) {
            first_audio_event_posted = true;
            ESP_LOGI(TAG, "Audio uplink: first send event posted (bits=0x%lx)",
                     static_cast<unsigned long>(bits));
        }
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
#if CONFIG_VOICELIFE_ENABLED
        if (GetDeviceState() == kDeviceStateListening) {
            const int64_t now = esp_timer_get_time();
            voicelife_capture_vad_speaking_.store(speaking);
            if (speaking) {
                voicelife_capture_speech_seen_.store(true);
                int64_t no_speech_time = 0;
                voicelife_capture_first_speech_us_.compare_exchange_strong(no_speech_time, now);
                voicelife_capture_silence_started_us_.store(0);
            } else if (voicelife_capture_speech_seen_.load()) {
                voicelife_capture_silence_started_us_.store(now);
            }
        }
#endif
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

#if CONFIG_VOICELIFE_ENABLED
    // VoiceLife runs on the device. Linx remains the ASR/agent/TTS transport;
    // no external #62 service is involved in tool execution.
    voicelife_service_ = std::make_unique<voicelife::VoiceLifeService>();
    if (voicelife_service_->Initialize()) {
        voicelife_service_->SetSpeechCallback(
            [this](const std::string& text) { return SendTextMessage(text); });
        voicelife::VoiceLifeMcpAdapter::Register(*voicelife_service_);
    } else {
        ESP_LOGE(TAG,
                 "VoiceLife local service is disabled because storage could not be initialized");
    }
#endif

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_VOICELIFE_AUDIO_QUIET;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);
        const bool trace_loop =
            GetDeviceState() == kDeviceStateListening || (bits & MAIN_EVENT_STATE_CHANGED) != 0;
        if (trace_loop) {
            ESP_LOGI(TAG, "Main loop: events=0x%lx state=%d", static_cast<unsigned long>(bits),
                     static_cast<int>(GetDeviceState()));
        }

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
            ESP_LOGI(TAG, "Main loop: state handler returned");
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
#if CONFIG_VOICELIFE_ENABLED
            // Do not close the transport until the last TTS packet has made it
            // through the decoder/output queue. This is the completion point
            // for an idle-state proactive reminder announcement.
            if (voicelife_announcement_close_pending_ && protocol_ &&
                protocol_->IsAudioChannelOpened()) {
                ESP_LOGI(TAG, "VoiceLife reminder playback drained; closing audio channel");
                voicelife_announcement_close_pending_ = false;
                voicelife_announcement_active_ = false;
                voicelife_announcement_audio_seen_ = false;
                voicelife_last_audio_time_us_.store(0);
                if (voicelife_audio_quiet_timer_handle_ != nullptr) {
                    esp_timer_stop(voicelife_audio_quiet_timer_handle_);
                }
                protocol_->CloseAudioChannel();
                SetDeviceState(kDeviceStateIdle);
            }
            if (voicelife_single_turn_close_pending_) {
                ESP_LOGI(TAG, "VoiceLife single-turn playback drained; returning to idle");
                voicelife_single_turn_close_pending_ = false;
                if (protocol_ && protocol_->IsAudioChannelOpened()) {
                    protocol_->CloseAudioChannel();
                }
                SetDeviceState(kDeviceStateIdle);
            }
#endif
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_VOICELIFE_AUDIO_QUIET) {
#if CONFIG_VOICELIFE_ENABLED
            MaybeFinishVoiceLifeAnnouncement();
#endif
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
#if CONFIG_VOICELIFE_ENABLED
            if (voicelife_announcement_active_ || voicelife_mcp_waiting_for_tts_ ||
                voicelife_capture_stop_sent_) {
                // Proactive reminders are receive-only. Interactive tool turns
                // also pause audio until Linx starts TTS so the MCP result has
                // priority on the shared WebSocket.
                const size_t dropped = audio_service_.DropPacketsFromSendQueue();
                if (dropped > 0) {
                    ESP_LOGI(TAG, "Dropped %u stale microphone packets during %s",
                             static_cast<unsigned>(dropped),
                             voicelife_announcement_active_
                                 ? "reminder"
                                 : (voicelife_mcp_waiting_for_tts_ ? "MCP continuation"
                                                                   : "capture finalization"));
                }
            } else
#endif
            {
                static uint32_t audio_packets_sent = 0;
                static bool first_send_event_logged = false;
                if (!first_send_event_logged) {
                    first_send_event_logged = true;
                    ESP_LOGI(TAG, "Audio uplink: main task received first send event");
                }
                size_t batch_size = 0;
                while (batch_size < kMaxAudioPacketsPerMainLoop) {
                    auto packet = audio_service_.PopPacketFromSendQueue();
                    if (!packet) {
                        break;
                    }
                    if (audio_packets_sent == 0) {
                        ESP_LOGI(TAG, "Audio uplink: sending first Opus packet (%u bytes)",
                                 static_cast<unsigned>(packet->payload.size()));
                    }
                    if (!protocol_ || !protocol_->SendAudio(std::move(packet))) {
                        const size_t dropped = audio_service_.DropPacketsFromSendQueue();
                        ESP_LOGE(TAG, "Audio uplink send failed; dropped %u queued packets",
                                 static_cast<unsigned>(dropped));
                        break;
                    }
                    ++batch_size;
                    ++audio_packets_sent;
                    if (audio_packets_sent == 1 || audio_packets_sent % 20 == 0) {
                        ESP_LOGI(TAG, "Audio uplink: sent %lu Opus packets",
                                 static_cast<unsigned long>(audio_packets_sent));
                    }
                }
                if (batch_size == kMaxAudioPacketsPerMainLoop) {
                    // Event bits coalesce. Re-arm the event so a continuously
                    // producing microphone cannot monopolize the application task.
                    xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
#if CONFIG_VOICELIFE_ENABLED
                MaybeStopVoiceLifeCapture();
#endif
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            ESP_LOGI(TAG, "Main loop: running %u scheduled task(s)",
                     static_cast<unsigned>(tasks.size()));
            size_t task_index = 0;
            for (auto& task : tasks) {
                ESP_LOGI(TAG, "Main loop: scheduled task %u begin",
                         static_cast<unsigned>(task_index));
                task();
                ESP_LOGI(TAG, "Main loop: scheduled task %u end",
                         static_cast<unsigned>(task_index));
                ++task_index;
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
#if CONFIG_VOICELIFE_ENABLED
            if (GetDeviceState() == kDeviceStateListening) {
                ESP_LOGI(TAG, "Main loop: VoiceLife tick begin");
            }
            if (voicelife_service_)
                voicelife_service_->Tick();
            if (GetDeviceState() == kDeviceStateListening) {
                ESP_LOGI(TAG, "Main loop: VoiceLife tick end");
            }
            // Linx does not always send a tts.stop frame for an injected text
            // turn. Check from the main task as a second completion path so a
            // reminder cannot leave the device permanently in speaking.
            MaybeFinishVoiceLifeAnnouncement();
            MaybeStopVoiceLifeCapture();
            MaybeCloseVoiceLifeConversation();
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

#if CONFIG_VOICELIFE_IM_ENABLED
void Application::StartVoiceLifeImSync() {
    if (voicelife_im_started_ || voicelife_service_ == nullptr)
        return;
    const std::string gateway_url = CONFIG_VOICELIFE_IM_GATEWAY_URL;
    const std::string gateway_token = CONFIG_VOICELIFE_IM_GATEWAY_TOKEN;
    if (gateway_url.empty() || gateway_token.empty()) {
        ESP_LOGW(TAG, "VoiceLife IM sync is enabled but Gateway URL/token is empty");
        return;
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "VoiceLife IM sync cannot start without a network interface");
        return;
    }
    voicelife_im_transport_ =
        std::make_unique<voicelife::VoiceLifeImHttpTransport>(network, gateway_url, gateway_token);
    voicelife::VoiceLifeImSync::Config config;
    config.device_id = SystemInfo::GetMacAddress();
    config.poll_seconds = CONFIG_VOICELIFE_IM_POLL_SECONDS;
    voicelife_im_sync_ = std::make_unique<voicelife::VoiceLifeImSync>(
        voicelife_service_.get(), voicelife_im_transport_.get(), std::move(config));
    voicelife_im_stop_.store(false);
    const BaseType_t created =
        xTaskCreate([](void* arg) { static_cast<Application*>(arg)->VoiceLifeImTask(); },
                    "voicelife_im", 6144, this, 2, &voicelife_im_task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start VoiceLife IM synchronization task");
        voicelife_im_sync_.reset();
        voicelife_im_transport_.reset();
        voicelife_im_task_handle_ = nullptr;
        return;
    }
    voicelife_im_started_ = true;
    ESP_LOGI(TAG, "VoiceLife IM synchronization started for device %s",
             SystemInfo::GetMacAddress().c_str());
}

void Application::VoiceLifeImTask() {
    while (!voicelife_im_stop_.load()) {
        uint32_t delay_seconds = CONFIG_VOICELIFE_IM_POLL_SECONDS;
        if (voicelife_im_sync_ != nullptr && !voicelife_im_sync_->PollOnce()) {
            uint32_t exponent = voicelife_im_sync_->ConsecutiveFailures();
            if (exponent > 4)
                exponent = 4;
            delay_seconds <<= exponent;
            if (delay_seconds > 60)
                delay_seconds = 60;
            ESP_LOGW(TAG, "VoiceLife IM sync failed (%u); retry in %u s: %s",
                     static_cast<unsigned>(voicelife_im_sync_->ConsecutiveFailures()),
                     static_cast<unsigned>(delay_seconds), voicelife_im_sync_->LastError().c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000U));
    }
    voicelife_im_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}
#endif

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
#if CONFIG_VOICELIFE_IM_ENABLED
    StartVoiceLifeImSync();
#endif
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
#if CONFIG_VOICELIFE_ENABLED
            if (ota_->LoadCachedWebsocketConfig()) {
                ESP_LOGW(TAG, "OTA check failed; continuing with cached Linx WebSocket settings");
                return;
            }
#endif
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
#if CONFIG_VOICELIFE_ENABLED
            if (voicelife_announcement_active_) {
                if (!voicelife_announcement_audio_seen_) {
                    voicelife_announcement_audio_seen_ = true;
                    ESP_LOGI(TAG, "VoiceLife reminder received TTS audio packet");
                }
                voicelife_last_audio_time_us_.store(esp_timer_get_time());
                if (voicelife_audio_quiet_timer_handle_ != nullptr) {
                    esp_timer_stop(voicelife_audio_quiet_timer_handle_);
                    esp_timer_start_once(voicelife_audio_quiet_timer_handle_,
                                         kVoiceLifeAudioQuietUs);
                }
            }
#endif
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
#if CONFIG_VOICELIFE_ENABLED
            voicelife_announcement_active_ = false;
            voicelife_announcement_close_pending_ = false;
            voicelife_announcement_audio_seen_ = false;
            voicelife_single_turn_close_pending_ = false;
            voicelife_followup_listen_pending_ = false;
            voicelife_wake_ack_pending_ = false;
            voicelife_response_needs_followup_.store(false);
            voicelife_business_response_completed_.store(false);
            voicelife_business_tools_allowed_.store(false);
            voicelife_mcp_waiting_for_tts_ = false;
            ResetVoiceLifeCapture();
            voicelife_conversation_deadline_us_.store(0);
            voicelife_last_audio_time_us_.store(0);
            if (voicelife_audio_quiet_timer_handle_ != nullptr) {
                esp_timer_stop(voicelife_audio_quiet_timer_handle_);
            }
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
#if CONFIG_VOICELIFE_ENABLED
                voicelife_response_needs_followup_.store(false);
#endif
                Schedule([this]() {
#if CONFIG_VOICELIFE_ENABLED
                    voicelife_conversation_deadline_us_.store(0);
                    voicelife_mcp_waiting_for_tts_ = false;
                    if (voicelife_announcement_active_) {
                        ESP_LOGI(TAG, "VoiceLife reminder TTS started");
                    }
#endif
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
#if CONFIG_VOICELIFE_ENABLED
                    if (voicelife_announcement_active_) {
                        ESP_LOGI(TAG, "VoiceLife reminder TTS stopped; waiting for playback drain");
                        // Keep the device in speaking state until queued TTS
                        // audio is drained; closing here can truncate the
                        // final Opus packets on a busy link.
                        voicelife_announcement_close_pending_ = true;
                        if (audio_service_.IsPlaybackIdle() && protocol_ &&
                            protocol_->IsAudioChannelOpened()) {
                            voicelife_announcement_close_pending_ = false;
                            voicelife_announcement_active_ = false;
                            voicelife_announcement_audio_seen_ = false;
                            voicelife_last_audio_time_us_.store(0);
                            protocol_->CloseAudioChannel();
                            SetDeviceState(kDeviceStateIdle);
                        }
                        return;
                    }
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        voicelife_business_tools_allowed_.store(false);
                        const bool wake_ack_pending = voicelife_wake_ack_pending_;
                        voicelife_wake_ack_pending_ = false;
                        const bool business_response_completed =
                            voicelife_business_response_completed_.exchange(false);
                        const bool needs_followup = voicelife::ShouldOpenFollowup(
                            wake_ack_pending, voicelife_response_needs_followup_.exchange(false),
                            business_response_completed);
                        if (listening_mode_ != kListeningModeManualStop && needs_followup) {
                            ESP_LOGI(TAG, "%s",
                                     wake_ack_pending
                                         ? "VoiceLife wake acknowledgement stopped; listening for "
                                           "command"
                                         : (business_response_completed
                                                ? "VoiceLife business response stopped; opening "
                                                  "bounded follow-up"
                                                : "VoiceLife response stopped; opening bounded "
                                                  "follow-up"));
                            voicelife_followup_listen_pending_ = true;
                            defer_listening_for_playback_ =
                                listening_mode_ == kListeningModeAutoStop;
                            if (!SetDeviceState(kDeviceStateListening)) {
                                voicelife_followup_listen_pending_ = false;
                                defer_listening_for_playback_ = false;
                            }
                            return;
                        }
                        ESP_LOGI(TAG, "VoiceLife response complete; closing after playback drain");
                        voicelife_single_turn_close_pending_ = true;
                        if (audio_service_.IsPlaybackIdle()) {
                            voicelife_single_turn_close_pending_ = false;
                            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                                protocol_->CloseAudioChannel();
                            }
                            SetDeviceState(kDeviceStateIdle);
                        }
                        return;
                    }
#endif
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            defer_listening_for_playback_ =
                                listening_mode_ == kListeningModeAutoStop;
                            if (!SetDeviceState(kDeviceStateListening)) {
                                defer_listening_for_playback_ = false;
                            }
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
#if CONFIG_VOICELIFE_ENABLED
                    if (!voicelife_announcement_active_ &&
                        voicelife::ResponseNeedsFollowup(text->valuestring)) {
                        voicelife_response_needs_followup_.store(true);
                    }
#endif
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
#if CONFIG_VOICELIFE_ENABLED
                const bool suppress_wake_transcript = voicelife::ShouldSuppressWakeTranscript(
                    voicelife_wake_ack_pending_, text->valuestring);
                if (!voicelife_announcement_active_) {
                    voicelife_business_response_completed_.store(false);
                    voicelife_stt_received_.store(true);
                    voicelife_business_tools_allowed_.store(true);
                    voicelife_conversation_deadline_us_.store(esp_timer_get_time() +
                                                              kVoiceLifeResponseTimeoutUs);
                    Schedule([this]() { StopVoiceLifeCapture("final STT received", false); });
                }
                if (suppress_wake_transcript) {
                    ESP_LOGI(TAG, "VoiceLife: suppressed synthetic wake transcript");
                    return;
                }
#endif
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
#if CONFIG_VOICELIFE_ENABLED
                const int64_t deadline = voicelife_conversation_deadline_us_.load();
                if (!voicelife_announcement_active_ && deadline > 0) {
                    voicelife_conversation_deadline_us_.store(esp_timer_get_time() +
                                                              kVoiceLifeMcpActivityTimeoutUs);
                }
                auto method = cJSON_GetObjectItem(payload, "method");
                auto params = cJSON_GetObjectItem(payload, "params");
                auto tool_name =
                    cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "name") : nullptr;
                if (!voicelife_announcement_active_ && cJSON_IsString(method) &&
                    strcmp(method->valuestring, "tools/call") == 0 && cJSON_IsString(tool_name) &&
                    voicelife::IsBusinessToolName(tool_name->valuestring) &&
                    !voicelife_business_tools_allowed_.load()) {
                    ESP_LOGW(TAG,
                             "Rejecting stale VoiceLife business tool before current-turn STT: %s",
                             tool_name->valuestring);
                    Schedule([this]() {
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->CloseAudioChannel(false);
                        }
                    });
                    return;
                }
                if (!voicelife_announcement_active_ && cJSON_IsString(method) &&
                    strcmp(method->valuestring, "tools/call") == 0 && cJSON_IsString(tool_name) &&
                    voicelife::IsBusinessToolName(tool_name->valuestring)) {
                    voicelife_business_response_completed_.store(true);
                }
                if (!voicelife_announcement_active_ && voicelife_stt_received_.load() &&
                    cJSON_IsString(method) && strcmp(method->valuestring, "tools/call") == 0) {
                    Schedule([this]() {
                        if (GetDeviceState() != kDeviceStateListening ||
                            voicelife_announcement_active_) {
                            return;
                        }
                        voicelife_mcp_waiting_for_tts_ = true;
                        audio_service_.EnableVoiceProcessing(false);
                        const size_t dropped = audio_service_.DropPacketsFromSendQueue();
                        ESP_LOGI(TAG,
                                 "VoiceLife MCP tool call: paused audio uplink, dropped %u packets",
                                 static_cast<unsigned>(dropped));
                    });
                } else if (!voicelife_announcement_active_ && !voicelife_stt_received_.load() &&
                           cJSON_IsString(method) &&
                           strcmp(method->valuestring, "tools/call") == 0) {
                    ESP_LOGI(TAG,
                             "VoiceLife MCP tool call before STT: keeping audio uplink active");
                }
#endif
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule(
                    [this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
#if CONFIG_VOICELIFE_ENABLED
    // Linx already receives the detected wake-word text below. Sending the
    // cached wake-word audio first can fill the small TCP send buffer before
    // MCP discovery completes, which blocks the application task and prevents
    // live microphone packets from being uploaded.
    ESP_LOGI(TAG, "VoiceLife: skipping cached wake-word audio burst");
#else
    audio_service_.EncodeWakeWord();
#endif

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_VOICELIFE_ENABLED
    // The Agent is constrained to answer a standalone wake invocation with
    // exactly "收到！". Keep this turn distinct so playback completion always
    // continues into bounded command capture instead of returning to idle.
    voicelife_wake_ack_pending_ = true;
    ESP_LOGI(TAG, "VoiceLife: requesting fixed wake acknowledgement");
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#elif CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    ESP_LOGI(TAG, "Handling state change: %s", DeviceStateMachine::GetStateName(new_state));
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;
    if (new_state != kDeviceStateListening) {
        defer_listening_for_playback_ = false;
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
#if CONFIG_VOICELIFE_ENABLED
            voicelife_mcp_waiting_for_tts_ = false;
            voicelife_wake_ack_pending_ = false;
            voicelife_response_needs_followup_.store(false);
            voicelife_business_response_completed_.store(false);
            voicelife_business_tools_allowed_.store(false);
            ResetVoiceLifeCapture();
#endif
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            ESP_LOGI(TAG, "Listening state ready: mode=%d processor=%s playback=%s",
                     static_cast<int>(listening_mode_),
                     audio_service_.IsAudioProcessorRunning() ? "on" : "off",
                     audio_service_.IsPlaybackIdle() ? "idle" : "busy");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // Only a TTS stop may defer auto-listening for playback drain.
                // A fresh user turn must start capture immediately even if a
                // startup/prompt packet left the playback tracker busy.
                const bool should_defer = defer_listening_for_playback_ &&
                                          listening_mode_ == kListeningModeAutoStop &&
                                          !audio_service_.IsPlaybackIdle();
                defer_listening_for_playback_ = false;
                if (should_defer) {
                    pending_listening_start_ = true;
                    ESP_LOGI(TAG, "Deferring TTS continuation until playback drains");
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

#if CONFIG_VOICELIFE_ENABLED
    voicelife_mcp_waiting_for_tts_ = false;
    ResetVoiceLifeCapture();
    voicelife_capture_started_us_.store(esp_timer_get_time());
#endif

#if CONFIG_VOICELIFE_ENABLED
    // Linx finalizes client-bounded captures reliably in realtime mode. Keep
    // listening_mode_ unchanged so this no-AEC board still behaves half-duplex
    // while TTS is playing.
    constexpr ListeningMode wire_listening_mode = kListeningModeRealtime;
#else
    const ListeningMode wire_listening_mode = listening_mode_;
#endif
    ESP_LOGI(TAG, "Audio uplink: sending listen.start (local=%d wire=%d)",
             static_cast<int>(listening_mode_), static_cast<int>(wire_listening_mode));
    protocol_->SendStartListening(wire_listening_mode);
    ESP_LOGI(TAG, "Audio uplink: enabling voice processing");
    audio_service_.EnableVoiceProcessing(true);
    ESP_LOGI(TAG, "Audio uplink: voice processing is %s",
             audio_service_.IsAudioProcessorRunning() ? "running" : "stopped");

    ESP_LOGI(TAG, "Audio uplink: configuring listening wake word");
    ConfigureWakeWordForListening();
    ESP_LOGI(TAG, "Audio uplink: listening pipeline ready");

#if CONFIG_VOICELIFE_ENABLED
    if (voicelife_followup_listen_pending_) {
        voicelife_followup_listen_pending_ = false;
        voicelife_conversation_deadline_us_.store(esp_timer_get_time() +
                                                  kVoiceLifeFollowupTimeoutUs);
        ESP_LOGI(TAG, "VoiceLife follow-up window opened for %lld ms",
                 static_cast<long long>(kVoiceLifeFollowupTimeoutUs / 1000));
    }
#endif

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // A detected wake word already disables its detector. Avoid applying the
    // same AFE control transition again while voice processing is starting.
    if (audio_service_.IsWakeWordRunning()) {
        audio_service_.EnableWakeWordDetection(false);
    } else {
        ESP_LOGI(TAG, "Wake word detection already stopped for listening");
    }
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
#if CONFIG_VOICELIFE_ENABLED
    // A proactive reminder is a receive-only Linx turn. Linx may still send
    // MCP discovery requests when the channel opens, but replying from the
    // application task can block the socket while the receive task is parsing
    // that same burst. MCP is not needed to deliver the reminder, so drop this
    // turn's discovery replies instead of stalling audio playback. Normal
    // interactive turns continue to use the existing MCP path.
    if (voicelife_announcement_active_) {
        ESP_LOGW(TAG, "Dropping MCP reply during VoiceLife reminder");
        return;
    }
#endif
    ESP_LOGI(TAG, "MCP reply queued (%u bytes)", static_cast<unsigned>(payload.size()));
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        ESP_LOGI(TAG, "MCP reply send begin (%u bytes)", static_cast<unsigned>(payload.size()));
        const bool sent = protocol_ && protocol_->SendMcpMessage(payload);
        if (!sent) {
            ESP_LOGE(TAG, "MCP reply send failed; closing audio channel");
            if (mcp_broadcast_callback_) {
                mcp_broadcast_callback_(payload);
            }
#if CONFIG_VOICELIFE_ENABLED
            voicelife_mcp_waiting_for_tts_ = false;
            voicelife_conversation_deadline_us_.store(0);
            audio_service_.DropPacketsFromSendQueue();
            if (protocol_) {
                protocol_->CloseAudioChannel(false);
            }
            SetDeviceState(kDeviceStateIdle);
#endif
            return;
        }
        ESP_LOGI(TAG, "MCP reply send end");
#if CONFIG_VOICELIFE_ENABLED
        if (voicelife_conversation_deadline_us_.load() > 0) {
            voicelife_conversation_deadline_us_.store(esp_timer_get_time() +
                                                      kVoiceLifeMcpResultTimeoutUs);
        }
#endif
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

#if CONFIG_VOICELIFE_ENABLED
bool Application::SendTextMessage(const std::string& text) {
    // Tick runs in the application main task. A proactive reminder is only
    // admitted from idle so it cannot interrupt a user conversation.
    if (!protocol_ || text.empty() || voicelife_announcement_active_ ||
        GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    voicelife_announcement_active_ = true;
    voicelife_announcement_close_pending_ = false;
    voicelife_announcement_audio_seen_ = false;
    voicelife_last_audio_time_us_.store(0);
    if (voicelife_audio_quiet_timer_handle_ != nullptr) {
        esp_timer_stop(voicelife_audio_quiet_timer_handle_);
    }
    listening_mode_ = kListeningModeManualStop;

    bool opened_here = false;
    if (!protocol_->IsAudioChannelOpened()) {
        opened_here = true;
        ESP_LOGI(TAG, "VoiceLife reminder opening Linx audio channel from idle");
        if (!SetDeviceState(kDeviceStateConnecting) || !protocol_->OpenAudioChannel()) {
            voicelife_announcement_active_ = false;
            voicelife_announcement_close_pending_ = false;
            SetDeviceState(kDeviceStateIdle);
            return false;
        }
    }

    // Connecting normally transitions to listening, which would send a
    // listen/start frame and enable the microphone. Proactive TTS needs a
    // receive-only turn, so use the explicit speaking state instead.
    if (GetDeviceState() == kDeviceStateConnecting && !SetDeviceState(kDeviceStateSpeaking)) {
        voicelife_announcement_active_ = false;
        voicelife_announcement_close_pending_ = false;
        if (opened_here)
            protocol_->CloseAudioChannel();
        return false;
    }
    if (GetDeviceState() == kDeviceStateIdle && !SetDeviceState(kDeviceStateSpeaking)) {
        voicelife_announcement_active_ = false;
        voicelife_announcement_close_pending_ = false;
        return false;
    }

    if (protocol_->SendTextMessage(text)) {
        ESP_LOGI(TAG, "VoiceLife reminder text accepted by Linx transport");
        return true;
    }

    voicelife_announcement_active_ = false;
    voicelife_announcement_close_pending_ = false;
    voicelife_last_audio_time_us_.store(0);
    if (protocol_->IsAudioChannelOpened())
        protocol_->CloseAudioChannel();
    return false;
}
#endif

#if CONFIG_VOICELIFE_ENABLED
void Application::MaybeFinishVoiceLifeAnnouncement() {
    if (!voicelife_announcement_active_ || !voicelife_announcement_audio_seen_ || !protocol_ ||
        !protocol_->IsAudioChannelOpened()) {
        return;
    }

    const int64_t last_audio_time = voicelife_last_audio_time_us_.load();
    if (last_audio_time <= 0)
        return;

    const int64_t quiet_for = esp_timer_get_time() - last_audio_time;
    if (quiet_for < kVoiceLifeAudioQuietUs)
        return;

    ESP_LOGI(TAG, "VoiceLife reminder checking playback queue after %lld ms quiet",
             static_cast<long long>(quiet_for / 1000));
    const bool playback_idle = audio_service_.IsPlaybackIdle();
    ESP_LOGI(TAG, "VoiceLife reminder playback queue check completed: %s",
             playback_idle ? "idle" : "busy");
    if (!playback_idle && quiet_for < kVoiceLifeAudioHardTimeoutUs) {
        if (voicelife_audio_quiet_timer_handle_ != nullptr &&
            !esp_timer_is_active(voicelife_audio_quiet_timer_handle_)) {
            esp_timer_start_once(voicelife_audio_quiet_timer_handle_, 500 * 1000);
        }
        return;
    }

    if (playback_idle) {
        ESP_LOGI(TAG, "VoiceLife reminder audio idle; closing Linx channel");
    } else {
        ESP_LOGW(TAG,
                 "VoiceLife reminder audio did not drain after %lld ms; forcing Linx channel close",
                 static_cast<long long>(quiet_for / 1000));
    }
    voicelife_announcement_active_ = false;
    voicelife_announcement_close_pending_ = false;
    voicelife_announcement_audio_seen_ = false;
    voicelife_last_audio_time_us_.store(0);
    if (voicelife_audio_quiet_timer_handle_ != nullptr) {
        esp_timer_stop(voicelife_audio_quiet_timer_handle_);
    }
    protocol_->CloseAudioChannel();
    // WebSocket teardown invokes the close callback asynchronously. Set the
    // state here as well so a transport that omits that callback cannot strand
    // the device in speaking.
    SetDeviceState(kDeviceStateIdle);
}

void Application::ResetVoiceLifeCapture() {
    voicelife_capture_stop_sent_ = false;
    voicelife_stt_received_.store(false);
    voicelife_capture_speech_seen_.store(false);
    voicelife_capture_vad_speaking_.store(false);
    voicelife_capture_started_us_.store(0);
    voicelife_capture_first_speech_us_.store(0);
    voicelife_capture_silence_started_us_.store(0);
}

void Application::StopVoiceLifeCapture(const char* reason, bool notify_server) {
    if (voicelife_capture_stop_sent_ || GetDeviceState() != kDeviceStateListening ||
        voicelife_announcement_active_) {
        return;
    }

    voicelife_capture_stop_sent_ = true;
    audio_service_.EnableVoiceProcessing(false);
    const size_t dropped = audio_service_.DropPacketsFromSendQueue();
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetStatus(kVoiceLifeProcessingStatus);
    }
    if (notify_server && protocol_ && protocol_->IsAudioChannelOpened()) {
        // Match the realtime start message used for Linx VoiceLife turns. Linx
        // uses this stop as the explicit end-of-input signal for final STT.
        protocol_->SendStopListening(kListeningModeRealtime);
    }
    // Once input is explicitly closed, final STT should arrive quickly. A
    // longer response deadline is installed only after STT is actually seen.
    const int64_t timeout_us =
        notify_server ? kVoiceLifeSttFinalizeTimeoutUs : kVoiceLifeResponseTimeoutUs;
    voicelife_conversation_deadline_us_.store(esp_timer_get_time() + timeout_us);
    ESP_LOGI(TAG, "VoiceLife capture finalized: %s, notify=%s, dropped=%u", reason,
             notify_server ? "yes" : "no", static_cast<unsigned>(dropped));
}

void Application::MaybeStopVoiceLifeCapture() {
    if (voicelife_capture_stop_sent_ || voicelife_stt_received_.load() ||
        GetDeviceState() != kDeviceStateListening || voicelife_announcement_active_ || !protocol_ ||
        !protocol_->IsAudioChannelOpened()) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    const int64_t started = voicelife_capture_started_us_.load();
    if (started <= 0)
        return;

    if (!voicelife_capture_speech_seen_.load()) {
        if (now - started >= kVoiceLifeNoSpeechTimeoutUs) {
            ESP_LOGI(TAG, "VoiceLife capture timed out without speech; returning to idle");
            protocol_->CloseAudioChannel(false);
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    const int64_t first_speech = voicelife_capture_first_speech_us_.load();
    const int64_t silence_started = voicelife_capture_silence_started_us_.load();
    const bool silence_complete = !voicelife_capture_vad_speaking_.load() && silence_started > 0 &&
                                  now - silence_started >= kVoiceLifeEndpointSilenceUs;
    const bool capture_limit_reached =
        first_speech > 0 && now - first_speech >= kVoiceLifeCaptureMaxUs;
    if (silence_complete) {
        StopVoiceLifeCapture("local VAD endpoint", true);
    } else if (capture_limit_reached) {
        StopVoiceLifeCapture("local capture limit", true);
    }
}

void Application::MaybeCloseVoiceLifeConversation() {
    const int64_t deadline = voicelife_conversation_deadline_us_.load();
    if (deadline <= 0 || esp_timer_get_time() < deadline ||
        GetDeviceState() != kDeviceStateListening || voicelife_announcement_active_ || !protocol_ ||
        !protocol_->IsAudioChannelOpened()) {
        return;
    }

    ESP_LOGI(TAG, "VoiceLife %s timed out; returning to idle",
             voicelife_stt_received_.load() ? "response" : "STT");
    voicelife_conversation_deadline_us_.store(0);
    voicelife_followup_listen_pending_ = false;
    voicelife_response_needs_followup_.store(false);
    voicelife_business_response_completed_.store(false);
    voicelife_business_tools_allowed_.store(false);
    // The timeout is a local fail-safe. Sending listen.stop over an already
    // congested socket can block the main task for seconds, so tear down the
    // channel directly and let the next wake word start a fresh session.
    protocol_->CloseAudioChannel(false);
    SetDeviceState(kDeviceStateIdle);
}
#endif

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
