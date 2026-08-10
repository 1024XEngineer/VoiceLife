#include "voicelife/runtime/runtime.h"

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
#include "nvs.h"
#include "voicelife/audio_esp/audio_board_profile.h"
#include "voicelife/audio_esp/esp32s3_audio_probe.h"
#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/esp_multinet_wake_detector.h"
#include "voicelife/linx/linx_speech_provider.h"
#include "voicelife/linx/linx_types.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_service.h"
#endif

#include "linx_mcp_bridge.h"
#include "linx_ota_bootstrap.h"
#include "schedule_mcp_tools.h"
#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";

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
        if (const Status secret_store = InitializeLinxSecretStore(); !secret_store.ok()) return secret_store;
        auto connection = BootstrapLinxOtaConfig();
        if (!connection.ok() || !connection.value.has_value()) {
            return connection.status;
        }
        linx_config_ = std::move(*connection.value);
        auto result = registry.Create("xrobot-websocket", {});
#else
        auto result = registry.Create("scaffold", {});
#endif
        if (!result.ok() || !result.value.has_value()) {
            return Status::Error(ErrorCode::kInternal, "无法创建语音 Provider: " + result.status.message);
        }
        provider_ = std::move(*result.value);

#ifdef ESP_PLATFORM
        audio_ports_ = std::make_unique<audio_esp::Esp32s3PcmAudioPorts>(audio_esp::VoiceLifePcbEsp32s3Profile());
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
                 "peak=%u mean_square=%llu signal=%d replay=%u min_heap=%u",
                 profile.id.c_str(), report.codec_control_required, report.i2c_bus_ready, report.es8311_ack,
                 report.es7210_ack, report.pca9557_ack, report.i2s_channels_ready, report.i2s_channels_started,
                 static_cast<unsigned>(report.bytes_written), static_cast<unsigned>(report.bytes_read),
                 static_cast<unsigned>(report.capture_samples), static_cast<unsigned>(report.nonzero_samples),
                 static_cast<unsigned>(report.changed_samples), static_cast<unsigned>(report.saturated_samples),
                 static_cast<unsigned long long>(report.saturation_ratio_ppm()), static_cast<unsigned>(report.peak_abs),
                 static_cast<unsigned long long>(report.mean_square()), report.capture_signal_detected(),
                 static_cast<unsigned>(report.replay_bytes_written),
                 static_cast<unsigned>(report.minimum_free_heap_bytes));
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
            wake_queue_ = xQueueCreate(4, sizeof(WakeRequest));
            if (wake_queue_ == nullptr) return Status::Error(ErrorCode::kInternal, "创建唤醒队列失败");
            const BaseType_t task_status =
                xTaskCreate(&Runtime::WakeTaskEntry, "voicelife_wake", 4096, this, 5, &wake_task_);
            if (task_status != pdPASS) return Status::Error(ErrorCode::kInternal, "创建唤醒控制任务失败");
        }
        const Status standby_status = wake_gate_->StartStandby();
        if (!standby_status.ok()) return standby_status;
        LogVoiceEvidence({.session_id = session_->config().session_id,
                          .generation = session_->generation(),
                          .event = "standby_ready",
                          .detail = {}});
        ESP_LOGI(kTag, "WAKE_STANDBY_READY=1 word=你好牛牛");
#endif
        return Status::Ok();
    }

   private:
#ifdef ESP_PLATFORM
    struct WakeRequest {
        char wake_word[32];
    };

    void QueueWakeWord(std::string_view wake_word) {
        if (wake_queue_ == nullptr) return;
        LogVoiceEvidence({.session_id = session_ ? session_->config().session_id : "",
                          .generation = session_ ? session_->generation() : 0,
                          .event = "wake_detected",
                          .detail = {}});
        WakeRequest request{};
        const std::size_t size =
            wake_word.size() < sizeof(request.wake_word) - 1 ? wake_word.size() : sizeof(request.wake_word) - 1;
        std::memcpy(request.wake_word, wake_word.data(), size);
        request.wake_word[size] = '\0';
        (void)xQueueSend(wake_queue_, &request, 0);
    }

    void QueueStandbyRecovery() {
        if (wake_queue_ == nullptr) return;
        const WakeRequest recovery{};
        (void)xQueueSend(wake_queue_, &recovery, 0);
    }

    void RestoreStandby() {
        if (!wake_gate_) return;
        const Status stop_status = wake_gate_->StopCapture();
        if (!stop_status.ok()) {
            ESP_LOGW(kTag, "本地待机恢复停止上行失败: %s", stop_status.message.c_str());
            return;
        }
        const Status standby_status = wake_gate_->StartStandby();
        if (!standby_status.ok()) {
            ESP_LOGW(kTag, "本地待机恢复失败: %s", standby_status.message.c_str());
            return;
        }
        LogVoiceEvidence({.session_id = session_ ? session_->config().session_id : "",
                          .generation = session_ ? session_->generation() : 0,
                          .event = "standby_ready",
                          .detail = {}});
        ESP_LOGI(kTag, "WAKE_STANDBY_READY=1");
    }

    static void WakeTaskEntry(void* context) { static_cast<Runtime*>(context)->WakeTask(); }

    void WakeTask() {
        WakeRequest request{};
        while (true) {
            if (xQueueReceive(wake_queue_, &request, portMAX_DELAY) != pdTRUE) continue;
            if (request.wake_word[0] == '\0') {
                RestoreStandby();
                continue;
            }
            if (!session_ || !provider_) continue;
            const Status notify = provider_->NotifyLocalWakeWord(request.wake_word);
            if (!notify.ok()) {
                ESP_LOGW(kTag, "本地唤醒通知 Linx 失败: %s", notify.message.c_str());
                RestoreStandby();
                continue;
            }
            const Status capture = session_->BeginCapture();
            if (!capture.ok()) {
                ESP_LOGW(kTag, "唤醒后开始采集失败: %s", capture.message.c_str());
                RestoreStandby();
            }
        }
    }

    void LogVoiceEvidence(const voice::VoiceEvidence& evidence) {
        // Evidence detail can contain STT text or service diagnostics. Emit
        // only lifecycle names and numeric counters needed for board review.
        if (evidence.event == "capture_started") {
            capture_started_us_.store(esp_timer_get_time());
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
        if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted" || evidence.event == "provider_error" ||
            evidence.event == "capture_stop_failed" || evidence.event == "tts_capture_stop_failed") {
            capture_started_us_.store(0);
        }
        if (evidence.event == "tts_stopped" || evidence.event == "tts_aborted") {
            QueueStandbyRecovery();
        }
        if (evidence.event == "transport_disconnected") QueueStandbyRecovery();
    }

    NvsSecretResolver linx_secrets_;
    mcp::McpServer mcp_server_;
    schedule::ScheduleService schedule_service_;
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
    std::atomic<int64_t> capture_started_us_{0};
#else
    ScaffoldAudioInput audio_input_;
    ScaffoldAudioOutput audio_output_;
#endif
    std::unique_ptr<voice::SpeechProviderAdapter> provider_;
    std::unique_ptr<voice::VoiceSession> session_;
};

}  // namespace

Status Start() {
    static Runtime runtime;
    return runtime.Start();
}

}  // namespace voicelife::runtime
