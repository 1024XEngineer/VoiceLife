#include "voicelife/runtime/runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <utility>

#ifdef ESP_PLATFORM
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "generated/farewell_pcm.h"
#include "generated/wake_ack_pcm.h"
#include "led_strip.h"
#include "nvs.h"
#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/esp_multinet_wake_detector.h"
#include "voicelife/display_esp/ssd1306_status_display.h"
#include "voicelife/im/esp_http_transport_factory.h"
#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_retry_policy.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/linx/linx_speech_provider.h"
#include "voicelife/linx/linx_types.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/schedule/schedule_rule_service.h"
#endif

#include "bootstrap/storage_bootstrap.h"
#include "im_runtime_bootstrap.h"
#include "linx_mcp_bridge.h"
#include "linx_ota_bootstrap.h"
#include "schedule_mcp_tools.h"
#include "voicelife/mcp/schedule_rule_mcp_tools.h"
#include "voicelife/voice/voice_interaction_controller.h"
#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";
constexpr uint32_t kWakeFeedbackMs = 800;
constexpr int64_t kWakeAckDisplayUs = 400 * 1000;
constexpr int64_t kVolumeOverlayUs = 1500 * 1000;
constexpr uint32_t kListenTimeoutMs = 15000;
constexpr uint32_t kFinalSttTimeoutMs = 5000;
#if CONFIG_VOICELIFE_IM_GATEWAY
constexpr bool kImGatewayEnabled = true;
#else
constexpr bool kImGatewayEnabled = false;
#endif

#if CONFIG_NVS_ENCRYPTION
Result<std::string> ReadNvsString(nvs_handle_t handle, const char* key) {
    size_t required = 0;
    esp_err_t error = nvs_get_str(handle, key, nullptr, &required);
    if (error != ESP_OK || required <= 1) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, std::string("缺少 Linx NVS 配置: ") + key);
    }
    std::string value(required, '\0');
    error = nvs_get_str(handle, key, value.data(), &required);
    if (error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "读取 Linx NVS 配置失败");
    }
    value.resize(required > 0 ? required - 1 : 0);
    if (value.empty()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, std::string("Linx NVS 配置为空: ") + key);
    }
    return Result<std::string>::Success(std::move(value));
}
#endif

class NvsSecretResolver final : public linx_esp::SecretResolverPort {
   public:
    Result<std::string> Resolve(std::string_view reference) override {
#if !CONFIG_NVS_ENCRYPTION
        (void)reference;
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "Linx token 解析需要启用 NVS encryption");
#else
        constexpr std::string_view prefix = "nvs://";
        if (reference.rfind(prefix, 0) != 0) {
            return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx token 引用必须使用 nvs://");
        }
        const std::string path(reference.substr(prefix.size()));
        const auto separator = path.find('/');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= path.size()) {
            return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx token 引用格式无效");
        }
        nvs_handle_t handle = 0;
        const esp_err_t open_error = nvs_open_from_partition(LinxSecretPartitionLabel(),
                                                             path.substr(0, separator).c_str(), NVS_READONLY, &handle);
        if (open_error != ESP_OK) {
            return Result<std::string>::Failure(ErrorCode::kNotFound, "Linx token NVS 命名空间不可用");
        }
        auto result = ReadNvsString(handle, path.substr(separator + 1).c_str());
        nvs_close(handle);
        return result;
#endif
    }
};

#if CONFIG_VOICELIFE_AUDIO_PORT_SMOKE
Status RunAudioPortSmoke() {
    const auto profile = audio_esp::VoiceLifePcbEsp32s3Profile();
    audio_esp::Esp32s3PcmAudioPorts ports(profile);
    std::atomic<std::size_t> captured_frames{0};
    std::atomic<std::size_t> nonzero_samples{0};
    ports.input().SetAudioSink([&](voice::AudioFrame frame) {
        captured_frames.fetch_add(1);
        for (std::size_t offset = 0; offset + 1 < frame.payload.size(); offset += 2) {
            if (frame.payload[offset] != 0 || frame.payload[offset + 1] != 0) {
                nonzero_samples.fetch_add(1);
            }
        }
        return Status::Ok();
    });

    auto capture_format = profile.capture_i2s.format;
    capture_format.frame_duration_ms = 60;
    auto playback_format = profile.playback_i2s.format;
    playback_format.frame_duration_ms = 60;

    Status status = ports.input().Open(capture_format);
    if (!status.ok()) {
        return status;
    }
    status = ports.output().Open(playback_format);
    if (!status.ok()) {
        ports.input().Close();
        return status;
    }
    status = ports.input().StartCapture(voice::VoiceMode::kManual);
    if (!status.ok()) {
        ports.output().Close();
        ports.input().Close();
        return status;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    status = ports.input().StopCapture();
    if (!status.ok()) {
        ports.output().Close();
        ports.input().Close();
        return status;
    }

    voice::AudioFrame tone;
    tone.format = playback_format;
    const std::size_t tone_samples =
        static_cast<std::size_t>(playback_format.sample_rate_hz) * playback_format.frame_duration_ms / 1000U;
    tone.payload.resize(tone_samples * sizeof(int16_t));
    for (std::size_t i = 0; i < tone_samples; ++i) {
        const int16_t sample = (i / 24U) % 2U == 0U ? 1200 : -1200;
        std::memcpy(tone.payload.data() + i * sizeof(sample), &sample, sizeof(sample));
    }
    status = ports.output().Push(tone);
    if (status.ok()) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    const auto stats = ports.stats();
    ESP_LOGI(kTag,
             "AUDIO_PORT_READY=1 AUDIO_PORT_CAPTURE_FRAMES=%u AUDIO_PORT_PLAYED_FRAMES=%u "
             "AUDIO_PORT_DROPPED_INPUT=%u AUDIO_PORT_REJECTED_OUTPUT=%u "
             "AUDIO_PORT_SHORT_READS=%u AUDIO_PORT_SHORT_WRITES=%u "
             "AUDIO_PORT_MIN_HEAP=%u AUDIO_PORT_SIGNAL=%d",
             static_cast<unsigned>(captured_frames.load()), static_cast<unsigned>(stats.played_frames),
             static_cast<unsigned>(stats.dropped_input_frames), static_cast<unsigned>(stats.rejected_output_frames),
             static_cast<unsigned>(stats.short_reads), static_cast<unsigned>(stats.short_writes),
             static_cast<unsigned>(stats.minimum_free_heap_bytes), nonzero_samples.load() > 0);

    ports.output().Close();
    ports.input().Close();
    if (!status.ok()) {
        return status;
    }
    if (captured_frames.load() == 0 || nonzero_samples.load() == 0) {
        return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 未检测到可变化的总线输入");
    }
    if (stats.played_frames == 0) {
        return Status::Error(ErrorCode::kUnavailable, "PCM Audio Port 未完成总线回放帧");
    }
    return Status::Ok();
}
#endif
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
    Status NotifyLocalWakeWord(std::string_view) override { return Status::Ok(); }
    Status Disconnect() override { return Status::Ok(); }
    Result<voice::VoiceAudioFormats> audio_formats() const override {
        voice::VoiceAudioFormats fmt;
        fmt.capture = voice::AudioFormat{};
        fmt.playback = voice::AudioFormat{};
        return Result<voice::VoiceAudioFormats>::Success(fmt);
    }
    const voice::CapabilityProfile& capabilities() const override { return profile_; }

   private:
    voice::CapabilityProfile profile_{"scaffold", {"streaming-asr", "tts"}};
};

class Runtime final {
   public:
    /** @brief 构造运行时并将日程服务绑定到持久化仓储。 */
    Runtime()
#ifdef ESP_PLATFORM
        : schedule_service_(storage_.GetScheduleRepository(), storage_.GetScheduleOperationRepository()),
          schedule_rule_service_(storage_.GetScheduleRuleRepository(), storage_.GetScheduleExceptionRepository(),
                                 storage_.GetScheduleRepository())
#endif
    {
        auto& registry = voice::SpeechProviderRegistry::Instance();
#ifdef ESP_PLATFORM
        init_status_ = RegisterScheduleMcpTools(mcp_server_, schedule_service_);
        if (init_status_.ok()) {
            ESP_LOGI(kTag, "MCP_TOOLS_READY count=4 names=schedule.create,schedule.update,schedule.delete,schedule.query");
        }
        if (init_status_.ok()) {
            init_status_ = mcp::RegisterScheduleRuleMcpTools(mcp_server_, schedule_rule_service_);
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
        const Status storage_status = storage_.Start();
        if (!storage_status.ok()) return storage_status;
#ifdef ESP_PLATFORM
        // 立创实战派 ESP32-S3 板载 WS2812 灯珠接 GPIO48（小智 BUILTIN_LED_GPIO）。
        // 上电未驱动时灯珠可能随机亮；用 RMT led_strip 初始化后立即 clear（GRB 全零）
        // 真正关闭灯珠。GPIO18 是 MCP 外接灯，不在本板默认范围。
        {
            led_strip_config_t strip_config = {};
            strip_config.strip_gpio_num = GPIO_NUM_48;
            strip_config.max_leds = 1;
            strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
            strip_config.led_model = LED_MODEL_WS2812;
            led_strip_rmt_config_t rmt_config = {};
            rmt_config.resolution_hz = 10 * 1000 * 1000;
            led_strip_handle_t strip = nullptr;
            if (led_strip_new_rmt_device(&strip_config, &rmt_config, &strip) == ESP_OK) {
                (void)led_strip_clear(strip);
                (void)led_strip_del(strip);
                ESP_LOGI(kTag, "BUILTIN_LED_GPIO48_CLEAR=1");
            } else {
                ESP_LOGW(kTag, "BUILTIN_LED_GPIO48_INIT_FAILED");
            }
            // clear 后把 GPIO48 配成输出低并保持：RMT 句柄删除后数据线若悬空，
            // WS2812 会因电平漂移重新点亮；拉低可锁定灯灭。
            gpio_config_t led_lock = {};
            led_lock.pin_bit_mask = 1ULL << GPIO_NUM_48;
            led_lock.mode = GPIO_MODE_OUTPUT;
            led_lock.pull_up_en = GPIO_PULLUP_DISABLE;
            led_lock.pull_down_en = GPIO_PULLDOWN_ENABLE;
            led_lock.intr_type = GPIO_INTR_DISABLE;
            if (gpio_config(&led_lock) == ESP_OK) {
                (void)gpio_set_level(GPIO_NUM_48, 0);
            }
        }
        if (const Status display_status = display_esp::InitializeStatusDisplay(); !display_status.ok()) {
            ESP_LOGW(kTag, "OLED 状态屏初始化失败: %s", display_status.message.c_str());
        }
        (void)display_esp::SetEmotion("thinking", "联网", {});
        if (const Status secret_store = InitializeLinxSecretStore(); !secret_store.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=secret_store code=%d", static_cast<int>(secret_store.code));
            (void)display_esp::SetEmotion("sad", "错误", {});
            return secret_store;
        }
        auto connection = BootstrapLinxOtaConfig();
        if (!connection.ok() || !connection.value.has_value()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=linx_bootstrap code=%d", static_cast<int>(connection.status.code));
            (void)display_esp::SetEmotion("sad", "错误", {});
            return connection.status;
        }
        (void)display_esp::SetEmotion("neutral", "连接", {});
        linx_config_ = std::move(*connection.value);
        // IM 的 SNTP、Gateway 探针和退避全部在独立任务中完成，语音启动路径不等待网络。
        StartImRuntime();
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
        audio_ports_ = std::make_unique<audio_esp::Esp32s3PcmAudioPorts>(audio_esp::VoiceLifePcbEsp32s3Profile());
        audio_ports_->SetOutputVolume(static_cast<uint8_t>(volume_));
        wake_detector_ = std::make_unique<audio_esp::EspMultiNetWakeDetector>();
        wake_gate_ = std::make_unique<voice::WakeGateAudioInput>(audio_ports_->input(), *wake_detector_);
        wake_gate_->SetWakeSink([this](std::string_view wake_word) { QueueWakeWord(wake_word); });
        session_ = std::make_unique<voice::VoiceSession>(
            *wake_gate_, audio_ports_->output(), *provider_,
            [this](const voice::VoiceEvidence& evidence) { LogVoiceEvidence(evidence); });
        voice::VoiceSessionConfig config;
        config.session_id = "voicelife-linx-session";
        config.provider_id = "xrobot-websocket";
        config.mode = voice::VoiceMode::kRealtime;
        config.audio.codec = voice::AudioCodec::kPcmS16Le;
        config.audio.sample_rate_hz = 16000;
        config.audio.channels = 1;
        config.audio.bits_per_sample = 16;
        config.audio.frame_duration_ms = 20;
#else
        session_ = std::make_unique<voice::VoiceSession>(audio_input_, audio_output_, *provider_);
        voice::VoiceSessionConfig config;
        config.session_id = "scaffold-session";
        config.provider_id = "scaffold";
#endif
        const Status session_status = session_->Start(config);
        if (!session_status.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=session_start code=%d", static_cast<int>(session_status.code));
            (void)display_esp::SetEmotion("sad", "错误", {});
            return session_status;
        }

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
        const Status audio_port_status = RunAudioPortSmoke();
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
        StartBoardControls();
#endif
        return Status::Ok();
    }

   private:
    StorageBootstrap storage_;
#ifdef ESP_PLATFORM
    void StartImRuntime() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        // 物理 USB 窗口也用于显式轮换 Quick Tunnel URL 与设备 Token，因此即使已有配置也启动。
        if (!StartImProvisioningTask()) {
            ESP_LOGW(kTag, "IM_PROVISION_TASK_FAILED=1");
        }
        bool expected = false;
        if (!im_lifecycle_started_.compare_exchange_strong(expected, true)) return;
        if (xTaskCreate(&Runtime::ImLifecycleTaskEntry, "voicelife_im_lifecycle", 8192, this, 3, &im_lifecycle_task_) !=
            pdPASS) {
            im_lifecycle_started_.store(false);
            ESP_LOGW(kTag, "IM_RUNTIME_TASK_FAILED=1");
        }
#else
        ESP_LOGI(kTag, "IM_RUNTIME_DISABLED=1");
#endif
    }

    static void ImLifecycleTaskEntry(void* context) { static_cast<Runtime*>(context)->ImLifecycleTask(); }

    void ImLifecycleTask() {
        im::ImRetryPolicy retry_policy;
        while (true) {
            Status status = Status::Error(ErrorCode::kUnavailable, "IM Runtime 等待网络");
            im::ImHttpResponse response{.status = im::ImTransportStatus::kNetworkFailure,
                                        .status_code = 0,
                                        .body = {},
                                        .message = "IM 前置条件未就绪"};

            if (im_readiness_.NetworkReady() && !im_readiness_.SystemTimeReady()) {
                status = SynchronizeSystemTime();
            }
            status = im_runtime_.Start();
            if (im_runtime_.state() == im::ImRuntimeState::kProbing) {
                response = im_runtime_.ProbeGateway();
                if (im_runtime_.state() != im::ImRuntimeState::kReady) {
                    status = Status::Error(ErrorCode::kUnavailable, "IM Gateway 认证探针失败");
                }
            }

            if (im_runtime_.state() == im::ImRuntimeState::kReady) {
                ESP_LOGI(kTag, "IM_RUNTIME_READY=1");
                break;
            }
            if (im_runtime_.state() == im::ImRuntimeState::kDisabled) {
                ESP_LOGI(kTag, "IM_RUNTIME_DISABLED=1");
                break;
            }
            if (im_runtime_.state() == im::ImRuntimeState::kUnconfigured) {
                ESP_LOGW(kTag, "IM_RUNTIME_DEGRADED=1 state=%d code=%d", static_cast<int>(im_runtime_.state()),
                         static_cast<int>(status.code));
                break;
            }

            ESP_LOGW(kTag, "IM_RUNTIME_DEGRADED=1 state=%d code=%d http_status=%d",
                     static_cast<int>(im_runtime_.state()), static_cast<int>(status.code), response.status_code);
            const auto delay_ms = retry_policy.NextDelay(response);
            if (!delay_ms.has_value()) break;
            ESP_LOGI(kTag, "IM_RUNTIME_RETRY attempt=%u delay_ms=%u", static_cast<unsigned>(retry_policy.attempts()),
                     static_cast<unsigned>(*delay_ms));
            vTaskDelay(pdMS_TO_TICKS(*delay_ms));
        }
        vTaskDelete(nullptr);
    }

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

    struct ButtonSample {
        gpio_num_t gpio = GPIO_NUM_NC;
        bool previous_pressed = false;
        bool long_fired = false;
        int64_t pressed_at_us = 0;
    };

    static constexpr gpio_num_t kBootButtonGpio = GPIO_NUM_0;
    static constexpr gpio_num_t kTouchButtonGpio = GPIO_NUM_47;
    static constexpr gpio_num_t kVolumeUpButtonGpio = GPIO_NUM_40;
    static constexpr gpio_num_t kVolumeDownButtonGpio = GPIO_NUM_39;
    static constexpr int64_t kLongPressUs = 2000000;

    static void BoardTaskEntry(void* context) { static_cast<Runtime*>(context)->BoardTask(); }

    void StartBoardControls() {
        const gpio_config_t config = {
            .pin_bit_mask = (1ULL << kBootButtonGpio) | (1ULL << kTouchButtonGpio) | (1ULL << kVolumeUpButtonGpio) |
                            (1ULL << kVolumeDownButtonGpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        buttons_[0].gpio = kBootButtonGpio;
        buttons_[1].gpio = kTouchButtonGpio;
        buttons_[2].gpio = kVolumeUpButtonGpio;
        buttons_[3].gpio = kVolumeDownButtonGpio;
        if (xTaskCreate(&BoardTaskEntry, "voicelife_buttons", 3072, this, 5, &button_task_) != pdPASS) {
            ESP_LOGW(kTag, "创建板级按键任务失败");
        }
    }

    void BoardTask() {
        while (true) {
            const int64_t now = esp_timer_get_time();
            for (std::size_t index = 0; index < buttons_.size(); ++index) {
                auto& button = buttons_[index];
                const bool pressed = gpio_get_level(button.gpio) == 0;
                if (pressed && !button.previous_pressed) {
                    button.pressed_at_us = now;
                    button.long_fired = false;
                    ESP_LOGI(kTag, "BUTTON_EVENT gpio=%d action=down", static_cast<int>(button.gpio));
                    if (index == 1) {
                        (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kPressDown);
                    }
                } else if (pressed && !button.long_fired && now - button.pressed_at_us >= kLongPressUs) {
                    button.long_fired = true;
                    ESP_LOGI(kTag, "BUTTON_EVENT gpio=%d action=long", static_cast<int>(button.gpio));
                    if (index == 2) {
                        SetVolume(100);
                    } else if (index == 3) {
                        SetVolume(0);
                    }
                } else if (!pressed && button.previous_pressed) {
                    ESP_LOGI(kTag, "BUTTON_EVENT gpio=%d action=up duration_ms=%lld", static_cast<int>(button.gpio),
                             static_cast<long long>((now - button.pressed_at_us) / 1000));
                    if (index == 0 && !button.long_fired) {
                        (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kToggleChat);
                    } else if (index == 1) {
                        (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kPressUp);
                    } else if (index == 2 && !button.long_fired) {
                        SetVolume(std::min(volume_ + 10, 100));
                    } else if (index == 3 && !button.long_fired) {
                        SetVolume(std::max(volume_ - 10, 0));
                    }
                    button.pressed_at_us = 0;
                }
                button.previous_pressed = pressed;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void SetVolume(int volume) {
        volume_ = std::clamp(volume, 0, 100);
        if (audio_ports_) audio_ports_->SetOutputVolume(volume_);
        // 音量通知 overlay：临时覆盖显示，1.5s 后恢复最新快照（不修改会话状态）。
        // 连续调音量只重置同一个计时器。
        char text[16] = {};
        std::snprintf(text, sizeof(text), "VOL:%d", volume_);
        (void)display_esp::SetEmotion("neutral", "音量", text);
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

    // 本地提示音：播放 popup.ogg 解码的 PCM（16kHz S16LE），按协商播放格式重采样。
    // 播放本地提示音（裸 PCM 16kHz mono）：直接入队，播放端口统一重采样到 24kHz。
    void PlayPrompt(const int16_t* pcm, size_t sample_count) {
        if (!audio_ports_ || pcm == nullptr || sample_count == 0) return;
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
        const Status push_status = audio_ports_->output().Push(frame);
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
        // WakeAck 显示租约：开麦（kWakeDetected 动作）不延迟；在租约期内
        // CommitSnapshot 把下行内容栏显示为“收到！”，到期后切换普通聆听。
        wake_ack_until_us_ = esp_timer_get_time() + kWakeAckDisplayUs;
        PlayWakeAck();
        const Status wake_status = HandleInteractionEvent(voice::VoiceInteractionEvent::kWakeDetected, wake_word);
        if (!wake_status.ok()) {
            // 唤醒被拒（如控制器不在 kStandby，可能是假待机残留）：MultiNet 已一次性
            // 停止，必须重启检测器，否则后续永远叫不醒。
            ESP_LOGW(kTag, "WAKE_REJECTED state=%d err=%s", static_cast<int>(interaction_.state()),
                     wake_status.message.c_str());
            if (wake_gate_) {
                (void)wake_gate_->StartStandby();
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

    // UTF-8 码点计数（滚动窗口按字符而非字节）。
    static size_t CountCodepoints(std::string_view text) {
        size_t count = 0;
        for (size_t i = 0; i < text.size();) {
            const uint8_t b = static_cast<uint8_t>(text[i]);
            size_t width = 1;
            if ((b & 0x80) == 0) {
                width = 1;
            } else if ((b & 0xe0) == 0xc0) {
                width = 2;
            } else if ((b & 0xf0) == 0xe0) {
                width = 3;
            } else if ((b & 0xf8) == 0xf0) {
                width = 4;
            }
            i += width;
            ++count;
        }
        return count;
    }

    // 下行长文本滚动：每 400ms 推进一个字符（按码点），滚动到末尾停止并停用定时器。
    // 音量 overlay 到期：递增 revision 触发 CommitSnapshot 恢复最新快照。
    static void VolumeOverlayEntry(void* context) {
        auto* self = static_cast<Runtime*>(context);
        self->volume_overlay_until_us_ = 0;
        ++self->snapshot_.revision;
        self->CommitSnapshot();
    }

    static void ScrollEntry(void* context) {
        auto* self = static_cast<Runtime*>(context);
        const size_t codepoints = CountCodepoints(self->scroll_content_);
        if (codepoints <= 6) {
            if (self->scroll_timer_ != nullptr) {
                (void)esp_timer_stop(self->scroll_timer_);
            }
            return;
        }
        ++self->scroll_offset_;
        // 末尾留 6 字符可见窗口后停止滚动（按码点，不用 UTF-8 字节数）。
        if (self->scroll_offset_ + 6 >= codepoints) {
            self->scroll_offset_ = codepoints - 6;
            if (self->scroll_timer_ != nullptr) {
                (void)esp_timer_stop(self->scroll_timer_);
            }
        }
        ++self->snapshot_.revision;
        self->CommitSnapshot();
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
        if (!wake_gate_) return;
        const Status stop_status = wake_gate_->StopCapture();
        if (!stop_status.ok()) {
            ESP_LOGW(kTag, "本地待机恢复停止上行失败: %s", stop_status.message.c_str());
            (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kFailure);
            return;
        }
        const Status standby_status = wake_gate_->StartStandby();
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
        if (audio_ports_) {
            for (int i = 0; i < 30 && !audio_ports_->output().IsIdle(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (first_standby_) {
            first_standby_ = false;
        } else {
            (void)display_esp::SetEmotion("happy", "牛牛走了！", {});
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
        const bool gate_ok = wake_gate_ && wake_gate_->standby();
        const bool atomic_ok = controller_ok && session_ok && gate_ok;
        ESP_LOGI(kTag, "WAKE_REARM controller=%d session=%d gate=%d atomic=%d", controller_ok ? 1 : 0,
                 session_ok ? 1 : 0, gate_ok ? 1 : 0, atomic_ok ? 1 : 0);
        if (atomic_ok) {
            snapshot_.phase = voice::VoiceInteractionState::kStandby;
            snapshot_.mood = voice::VoiceMood::kNeutral;
            const time_t now = time(nullptr);
            if (now > 1600000000) {
                std::tm local{};
                localtime_r(&now, &local);
                char clock_text[8] = {};
                std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local.tm_hour, local.tm_min);
                snapshot_.status_text = clock_text;
            } else {
                snapshot_.status_text = PhaseStatusText(voice::VoiceInteractionState::kStandby);
            }
            snapshot_.content_text.clear();
            snapshot_.role = voice::VoiceContentRole::kNone;
            ++snapshot_.revision;
            CommitSnapshot();
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
                if (audio_ports_) {
                    for (int i = 0; i < 30 && !audio_ports_->output().IsIdle(); ++i) {
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

    // 显示模型：由会话阶段推导可见状态，仅在 revision 变化时提交渲染器。
    // phase→状态栏文本 与 mood 映射集中在此，不再散落在各事件分支。
    static std::string_view PhaseStatusText(voice::VoiceInteractionState state) {
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

    static voice::VoiceMood PhaseMood(voice::VoiceInteractionState state) {
        switch (state) {
            case voice::VoiceInteractionState::kListening:
            case voice::VoiceInteractionState::kFinalizing:
            case voice::VoiceInteractionState::kThinking:
            case voice::VoiceInteractionState::kReconnecting:
                return voice::VoiceMood::kThinking;
            case voice::VoiceInteractionState::kSpeaking:
                return voice::VoiceMood::kSpeaking;
            case voice::VoiceInteractionState::kInterrupting:
                return voice::VoiceMood::kSurprised;
            case voice::VoiceInteractionState::kError:
                return voice::VoiceMood::kSad;
            default:
                return voice::VoiceMood::kNeutral;
        }
    }

    void CommitSnapshot() {
        if (snapshot_.revision == last_rendered_revision_) {
            return;
        }
        last_rendered_revision_ = snapshot_.revision;
        // 渲染器：牛头表情 + 上行状态栏 + 下行内容栏（角色文本）。
        std::string mood_key;
        switch (snapshot_.mood) {
            case voice::VoiceMood::kHappy:
                mood_key = "happy";
                break;
            case voice::VoiceMood::kSad:
                mood_key = "sad";
                break;
            case voice::VoiceMood::kThinking:
                mood_key = "thinking";
                break;
            case voice::VoiceMood::kSurprised:
                mood_key = "surprised";
                break;
            case voice::VoiceMood::kSpeaking:
                mood_key = "speaking";
                break;
            case voice::VoiceMood::kAngry:
                mood_key = "angry";
                break;
            default:
                mood_key = "neutral";
                break;
        }
        (void)display_esp::SetEmotion(mood_key, snapshot_.status_text, snapshot_.content_text, scroll_offset_);
    }

    Status HandleInteractionEvent(voice::VoiceInteractionEvent event, std::string_view wake_word = {}) {
        const auto transition = interaction_.Handle(event);
        if (!transition.ok() || !transition.value.has_value()) {
            ESP_LOGW(kTag, "忽略乱序板端交互事件=%d: %s", static_cast<int>(event), transition.status.message.c_str());
            return transition.status;
        }
        // 会话阶段 → 显示模型快照：状态栏文本 + 表情由阶段派生。
        snapshot_.phase = interaction_.state();
        snapshot_.mood = PhaseMood(snapshot_.phase);
        // 空闲态显示当前时间（若服务端时间已初始化，约 2020 年后），否则显示状态词。
        const time_t now = time(nullptr);
        const bool clock_synced = now > 1600000000;  // 2020-09-13 之后的真实时间
        if (snapshot_.phase == voice::VoiceInteractionState::kStandby && clock_synced) {
            std::tm local{};
            localtime_r(&now, &local);
            char clock_text[8] = {};
            std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local.tm_hour, local.tm_min);
            snapshot_.status_text = clock_text;
        } else {
            snapshot_.status_text = PhaseStatusText(snapshot_.phase);
        }
        // 事件驱动的内容角色切换：
        // - kIntentReceived（STT）：内容栏显示用户语音，角色 user
        // - kTtsStarted：内容栏保持/显示助手文本，角色 assistant
        // - 会话结束/回待机：清空内容栏
        // WakeAck 租约：唤醒后短窗（400ms）内显示“收到！”，不阻塞开麦。
        if (event == voice::VoiceInteractionEvent::kWakeDetected &&
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
        ++snapshot_.revision;
        CommitSnapshot();
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
        const auto stats = audio_ports_ ? audio_ports_->stats() : audio_esp::AudioPortStats{};
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
                snapshot_.content_text = evidence.detail;
                snapshot_.role = voice::VoiceContentRole::kAssistant;
                snapshot_.status_text = "说话中";
                snapshot_.mood = voice::VoiceMood::kSpeaking;
                // 新内容：重置滚动窗口；超宽（>6 字符约 108px）启动滚动定时器。
                scroll_content_ = evidence.detail;
                scroll_offset_ = 0;
                if (scroll_timer_ == nullptr) {
                    esp_timer_create_args_t args = {};
                    args.callback = &ScrollEntry;
                    args.arg = this;
                    args.name = "voicelife_scroll";
                    (void)esp_timer_create(&args, &scroll_timer_);
                }
                if (scroll_timer_ != nullptr) {
                    (void)esp_timer_stop(scroll_timer_);
                    if (CountCodepoints(scroll_content_) > 6) {
                        (void)esp_timer_start_periodic(scroll_timer_, 400 * 1000ULL);
                    }
                }
                ++snapshot_.revision;
                CommitSnapshot();
            }
        } else if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted") {
            CancelListenTimer();
            if (scroll_timer_ != nullptr) {
                (void)esp_timer_stop(scroll_timer_);
            }
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
                    snapshot_.content_text.clear();
                    snapshot_.role = voice::VoiceContentRole::kNone;
                    ++snapshot_.revision;
                    CommitSnapshot();
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
    NvsImSecretStore im_secret_store_;
    im::StoredImConfigProvider im_config_{im_secret_store_, kImGatewayEnabled};
    EspImRuntimeReadiness im_readiness_;
    im::ImRuntime im_runtime_{im_config_, im_config_, im_readiness_,
                              [](const std::string& origin) { return im::CreateEspHttpTransport(origin); }};
    std::atomic_bool im_lifecycle_started_{false};
    TaskHandle_t im_lifecycle_task_ = nullptr;
    mcp::McpServer mcp_server_;
    schedule::ScheduleService schedule_service_;
    schedule::ScheduleRuleService schedule_rule_service_;
    Status init_status_ = Status::Ok();
    linx::LinxJsonCodec linx_codec_;
    linx::LinxConnectionConfig linx_config_;
    std::unique_ptr<linx_esp::EspWebSocketTransport> linx_transport_ =
        std::make_unique<linx_esp::EspWebSocketTransport>(linx_secrets_);
    std::unique_ptr<audio_esp::Esp32s3PcmAudioPorts> audio_ports_;
    std::unique_ptr<audio_esp::EspMultiNetWakeDetector> wake_detector_;
    std::unique_ptr<voice::WakeGateAudioInput> wake_gate_;
    QueueHandle_t wake_queue_ = nullptr;
    TaskHandle_t wake_task_ = nullptr;
    TaskHandle_t button_task_ = nullptr;
    std::array<ButtonSample, 4> buttons_{};
    int volume_ = 70;
    std::atomic<int64_t> capture_started_us_{0};
    bool first_standby_ = true;
    std::string stt_display_text_;
    // 下行内容滚动窗口起始字符（0=从头）；新内容重置，超宽时定时推进。
    size_t scroll_offset_ = 0;
    std::string scroll_content_;
    esp_timer_handle_t scroll_timer_ = nullptr;
    // 本轮是否为终止回合（用户说“再见/拜拜”等）：播报结束后不进入 follow-up。
    bool terminal_turn_ = false;
    // 最近唤醒词与其发生时刻（抑制唤醒词被服务端回传为 STT）。
    std::string last_wake_word_;
    int64_t last_wake_at_ = 0;
    // WakeAck 显示租约截止时刻（esp_timer_us）：到期前下行栏显示“收到！”。
    int64_t wake_ack_until_us_ = 0;
    // 音量 overlay 截止时刻（esp_timer_us）：到期后恢复最新快照。
    int64_t volume_overlay_until_us_ = 0;
    esp_timer_handle_t volume_overlay_timer_ = nullptr;
    // 显示模型快照：会话阶段 → 可见状态的推导结果；revision 驱动增量重绘。
    voice::DisplaySnapshot snapshot_;
    uint64_t last_rendered_revision_ = 0;
    esp_timer_handle_t listen_timer_ = nullptr;
#else
    ScaffoldAudioInput audio_input_;
    ScaffoldAudioOutput audio_output_;
#endif
    voice::VoiceInteractionController interaction_;
    std::unique_ptr<voice::SpeechProviderAdapter> provider_;
    std::unique_ptr<voice::VoiceSession> session_;

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
