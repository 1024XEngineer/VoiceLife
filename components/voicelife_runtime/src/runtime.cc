#include "voicelife/runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#ifdef ESP_PLATFORM
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>

#include "esp_action_stream_transport.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "voicelife/audio_esp/esp_opus_codec_strategy.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/json.h"
#include "voicelife/im/esp_http_transport_factory.h"
#include "voicelife/im/im_action_channel.h"
#include "voicelife/im/im_binding_use_case.h"
#include "voicelife/im/im_config_store.h"
#include "voicelife/im/im_retry_policy.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/linx/linx_speech_provider.h"
#include "voicelife/linx/linx_types.h"
#include "voicelife/linx_esp/esp_websocket_transport.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_reminder_service.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/timing_esp/esp_timing_runtime.h"
#endif

#include "bootstrap/storage_bootstrap.h"
#include "im_binding_mcp_tools.h"
#include "im_binding_polling_lease.h"
#include "im_binding_presentation.h"
#include "im_runtime_bootstrap.h"
#include "linx_mcp_bridge.h"
#include "linx_ota_bootstrap.h"
#include "mcp_worker_policy.h"
#include "schedule_reminder_im_adapter.h"
#include "serial_voice_test.h"
#include "voicelife/application/interaction_orchestrator.h"
#include "voicelife/mcp/schedule_mcp_tools.h"
#include "voicelife/runtime_esp/esp_interaction_task_host.h"
#include "voicelife/voice/display_snapshot.h"
#include "voicelife/voice/voice_interaction_controller.h"
#include "voicelife/voice/voice_ports.h"
#include "voicelife/voice/voice_session.h"

namespace voicelife::runtime {
namespace {

#ifdef ESP_PLATFORM
constexpr char kTag[] = "VoiceLifeRuntime";
constexpr int64_t kWakeAckDisplayUs = 400 * 1000;
constexpr int64_t kVolumeOverlayUs = 1500 * 1000;
constexpr uint32_t kTerminalWakeGuardMs = 8000;
// 唤醒或 follow-up 后的首次开口等待：6 秒足以让用户听清提示并开口，
// 又不会让无输入回合长时间占住 UI。说话后的端点与最终 STT 分别处理。
constexpr uint32_t kListenStartTimeoutMs = 6000;
constexpr uint32_t kFinalSttTimeoutMs = 5000;
// 确认语音不能拖慢首轮交互。Linx 若未在此窗口内送达首段 PCM，保留
// 原音色的最佳策略是跳过迟到确认并立即开麦，而不是播放一段过时的“收到”。
constexpr uint32_t kWakeAckFirstAudioTimeoutMs = 1800;
constexpr int64_t kVoiceActionReportRetryUs = 5 * 1000 * 1000;

std::string FormatReminderIso(schedule::DateTime value) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
    const std::time_t timestamp = static_cast<std::time_t>(seconds);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &timestamp) != 0) return "1970-01-01T00:00:00.000Z";
#else
    if (gmtime_r(&timestamp, &utc) == nullptr) return "1970-01-01T00:00:00.000Z";
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0) return "1970-01-01T00:00:00.000Z";
    return std::string(buffer) + ".000Z";
}
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
    Status Push(voice::AudioFrame) override { return Status::Ok(); }
    Status Flush() override { return Status::Ok(); }
    bool IsIdle() const override { return true; }
    void Close() override {}
};

class ScaffoldSpeechProvider final : public voice::SpeechProviderAdapter {
   public:
    Status Connect(const voice::VoiceSessionConfig&, voice::VoiceEventSink) override { return Status::Ok(); }
    Status StartCapture(voice::VoiceMode) override { return Status::Ok(); }
    Status StopCapture() override { return Status::Ok(); }
    Status SendAudio(voice::AudioFrame) override { return Status::Ok(); }
    Status Abort(std::string_view) override { return Status::Ok(); }
    Status Speak(std::string_view) override { return Status::Ok(); }
    Status NotifyLocalWakeWord(std::string_view, std::string_view = {}) override { return Status::Ok(); }
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

#ifdef ESP_PLATFORM
class ReminderSpeech final : public schedule::ScheduleReminderSpeechPort {
   public:
    explicit ReminderSpeech(std::function<Status(std::string_view)> speak) : speak_(std::move(speak)) {}

    Status SpeakScheduleReminder(std::string_view text) override { return speak_(text); }

   private:
    std::function<Status(std::string_view)> speak_;
};
#endif

class Runtime final {
   public:
    /** @brief 构造运行时并将日程服务绑定到持久化仓储。 */
    Runtime()
#ifdef ESP_PLATFORM
        : schedule_operation_service_(storage_.GetScheduleOperationRepository()),
          schedule_service_(storage_.GetScheduleRepository(), &schedule_operation_service_),
          schedule_rule_service_(storage_.GetScheduleRuleRepository(), storage_.GetScheduleExceptionRepository(),
                                 storage_.GetScheduleRepository())
#endif
    {
        auto& registry = voice::SpeechProviderRegistry::Instance();
#ifdef ESP_PLATFORM
        init_status_ =
            RegisterImBindingMcpTools(mcp_server_, binding_use_case_, [this](const im::BindingResult& result) {
                EnqueueBindingResult(result);
                if (result.state == im::BindingState::kPending) StartBindingPolling(result.generation);
            });
        registry.Register("xrobot-websocket", linx::LinxSpeechProviderAdapter::DefaultCapabilities(), [this]() {
            return std::make_unique<linx::LinxSpeechProviderAdapter>(
                *linx_transport_, linx_codec_, linx_config_, linx::LinxSpeechProviderAdapter::DefaultCapabilities(),
                [this](std::string_view payload, std::string_view session_id) {
                    return HandleMcpRequest(payload, session_id);
                },
                audio_esp::CreateEspOpusCodecStrategy());
        });
#endif
        registry.Register("scaffold", voice::CapabilityProfile{"scaffold", {"streaming-asr", "tts"}},
                          []() { return std::make_unique<ScaffoldSpeechProvider>(); });
    }

    Status Start(PlatformAssembly& assembly) {
        assembly_ = &assembly;
        const auto fail_startup = [this](Status status) {
#ifdef ESP_PLATFORM
            if (assembly_ != nullptr) {
                assembly_->wake_gate().Close();
                assembly_->audio_output().Close();
            }
            StopMcpWorker();
            StopEventLoop();
            StopScheduleReminderRuntime();
#endif
            return status;
        };
        auto& registry = voice::SpeechProviderRegistry::Instance();
        if (!init_status_.ok()) return init_status_;
        voice::VoiceSessionConfig config;
#ifdef ESP_PLATFORM
        config.session_id = "voicelife-linx-session";
        config.provider_id = "xrobot-websocket";
        // SparkBot has no playback reference channel or AEC. Match the
        // Xiaozhi SparkBot default: server VAD closes an auto-stop turn after
        // playback drains, while realtime is reserved for AEC-capable boards.
        config.mode = voice::VoiceMode::kAuto;
        config.audio.codec = voice::AudioCodec::kPcmS16Le;
        config.audio.sample_rate_hz = 16000;
        config.audio.channels = 1;
        config.audio.bits_per_sample = 16;
        config.audio.frame_duration_ms = 20;
#else
        config.session_id = "scaffold-session";
        config.provider_id = "scaffold";
#endif
        // NVS 加密初始化会创建 AES-XTS 中断处理器。必须在日程/MCP Schema
        // 和任务创建前完成，避免启动分配峰值让底层中断分配器收到损坏状态。
        {
            esp_err_t nvs_error = nvs_flash_init();
            if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                (void)nvs_flash_erase();
                nvs_error = nvs_flash_init();
            }
            if (nvs_error != ESP_OK) {
                ESP_LOGE(kTag, "STARTUP_ERROR stage=nvs_flash_init code=%d", static_cast<int>(nvs_error));
                return Status::Error(ErrorCode::kInternal, "主 NVS 初始化失败");
            }
            ESP_LOGI(kTag, "NVS_READY=1");
        }
#ifdef ESP_PLATFORM
        // IM lifecycle reads encrypted NVS and must keep an internal stack. Reserve
        // it before storage, MCP, Wi-Fi, TLS, and audio fragment the internal heap;
        // the task remains blocked until the Linx startup path explicitly activates it.
        ReserveImRuntimeTask();
        // Strong-reminder actions can enter SQLite/FATFS and temporarily disable
        // flash cache. Their stack must live in DRAM, so reserve the worker before
        // storage startup and the later display/audio/network allocation peaks.
        if (!StartReminderActionWorker()) {
            return Status::Error(ErrorCode::kInternal, "创建提醒动作工作任务失败");
        }
#endif
        const Status storage_status = storage_.Start();
        if (!storage_status.ok()) {
#ifdef ESP_PLATFORM
            (void)StopReminderActionWorker();
#endif
            return storage_status;
        }
#ifdef ESP_PLATFORM
        // 显示在连接、MCP Schema 和提醒任务之前启动。这样既能尽早给出设备反馈，
        // 也让 SPI 中断分配不与大批量动态对象构造交错。
        assembly_->InitializeBoardLeds();
        if (const Status display_status = assembly_->Start(); !display_status.ok()) {
            ESP_LOGE(kTag, "STARTUP_ERROR stage=display_start code=%d msg=%s", static_cast<int>(display_status.code),
                     display_status.message.c_str());
            return display_status;
        }
        ESP_LOGI(kTag, "DISPLAY_READY=1");
        ESP_LOGI(kTag, "STARTUP_STAGE=timing_create_begin main_stack_high_water=%u",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        auto timing_runtime = timing_esp::EspTimingTaskRuntime::Create();
        ESP_LOGI(kTag, "STARTUP_STAGE=timing_create_end main_stack_high_water=%u",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        if (!timing_runtime.ok() || !timing_runtime.value.has_value()) {
            const Status failure = timing_runtime.ok()
                                       ? Status::Error(ErrorCode::kInternal, "ESP Timing 运行时创建结果为空")
                                       : timing_runtime.status;
            ESP_LOGE(kTag, "STARTUP_ERROR stage=timing_runtime code=%d msg=%s", static_cast<int>(failure.code),
                     failure.message.c_str());
            return failure;
        }
        timing_runtime_ = std::move(*timing_runtime.value);
        reminder_speech_ = std::make_unique<ReminderSpeech>([this](std::string_view text) {
            return QueueSystemSpeech(text) ? Status::Ok()
                                           : Status::Error(ErrorCode::kUnavailable, "系统提醒播报请求未进入板级队列");
        });
        reminder_notification_ = std::make_unique<ImScheduleReminderNotification>(
            im_runtime_, [this](im::ActionWindow window) { EnqueueReminderActionWindow(std::move(window)); });
        schedule_reminder_service_ = std::make_unique<schedule::ScheduleReminderService>(
            storage_.GetScheduleRepository(), storage_.GetScheduleReminderTaskRepository(), schedule_service_,
            schedule_rule_service_, *timing_runtime_, *reminder_speech_, reminder_notification_.get());
        reminder_action_executor_ = std::make_unique<ImScheduleReminderActionExecutor>(*schedule_reminder_service_);
        // 先注册 MCP 工具契约，再启动提醒任务。工具注册会建立参数 Schema 和
        // handler 闭包，属于一次性启动分配；提醒运行时随后启动，避免两者在
        // 内部堆上同时竞争初始化峰值。回调只有在 MCP worker 启动后才会执行。
        if (!schedule_mcp_registered_) {
            init_status_ = mcp::RegisterScheduleMcpTools(mcp_server_, schedule_service_, schedule_rule_service_,
                                                         schedule_operation_service_, schedule_reminder_service_.get(),
                                                         {.runtime = &im_runtime_});
            if (!init_status_.ok()) return fail_startup(init_status_);
            schedule_mcp_registered_ = true;
            // MCP worker 只产生绑定结果；轮询与 OLED/TTS 均由各自受控任务处理。
            ESP_LOGI(
                kTag,
                "MCP_TOOLS_READY count=8 names=schedule.create,schedule.query,schedule.update,schedule.delete,"
                "schedule.operation_query,schedule.reminder_acknowledge,schedule.reminder_snooze,im.binding.start");
        }
        const Status reminder_status = schedule_reminder_service_->Start();
        if (!reminder_status.ok()) {
            ESP_LOGE(kTag, "STARTUP_ERROR stage=schedule_reminder code=%d msg=%s",
                     static_cast<int>(reminder_status.code), reminder_status.message.c_str());
            return fail_startup(reminder_status);
        }
        ESP_LOGI(kTag, "SCHEDULE_REMINDER_READY=1");
        // 立创实战派 ESP32-S3 板载 WS2812 灯珠接 GPIO48（小智 BUILTIN_LED_GPIO）。
        // 显示启动后立即启动唯一的交互/显示语义写者。此后的启动、网络、音量
        // 和会话事件均只投递到该循环，不允许 Runtime 直接 Render。
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            control_event_queue_.clear();
            best_effort_event_queue_.clear();
            event_loop_stop_ = false;
            event_loop_stopped_ = false;
        }
        if (xTaskCreate(&Runtime::EventLoopTaskEntry, "voicelife_interaction", 8192, this, 5, &event_task_) != pdPASS) {
            return Status::Error(ErrorCode::kInternal, "创建交互事件循环任务失败");
        }
        if (const Status mcp_worker = StartMcpWorker(); !mcp_worker.ok()) {
            return fail_startup(mcp_worker);
        }
#ifdef ESP_PLATFORM
        if (wake_queue_ == nullptr) {
            wake_queue_ = xQueueCreate(4, sizeof(BoardRequest));
            if (wake_queue_ == nullptr) return fail_startup(Status::Error(ErrorCode::kInternal, "创建唤醒队列失败"));
            // WakeTask loads the local wake model through esp_partition_mmap(), which disables the
            // cache and therefore requires an internal-memory stack. Reserve it before audio and
            // network initialization fragment the remaining internal heap.
            const BaseType_t task_status =
                xTaskCreate(&Runtime::WakeTaskEntry, "voicelife_wake", 4096, this, 5, &wake_task_);
            if (task_status != pdPASS) return fail_startup(Status::Error(ErrorCode::kInternal, "创建唤醒控制任务失败"));
        }
        // I2S DMA buffers require contiguous internal memory. Open the fixed PCM
        // ports before Wi-Fi/TLS startup; VoiceSession repeats Open after hello
        // to validate that the negotiated format matches this prepared device.
        assembly_->SetOutputVolume(static_cast<uint8_t>(volume_));
        if (const Status input_status = assembly_->wake_gate().Open(config.audio); !input_status.ok()) {
            return fail_startup(input_status);
        }
        if (const Status output_status = assembly_->audio_output().Open(config.audio); !output_status.ok()) {
            return fail_startup(output_status);
        }
        ESP_LOGI(kTag, "AUDIO_PREPARED=1");
#endif
        ShowDisplay(voice::VoiceMood::kConnecting, "联网", "");
        if (const Status secret_store = InitializeLinxSecretStore(); !secret_store.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=secret_store code=%d", static_cast<int>(secret_store.code));
            ShowDisplay(voice::VoiceMood::kSad, "错误", "");
            return fail_startup(secret_store);
        }
        // USB IM provisioning shares the USB-Serial/JTAG input with the Linx Wi-Fi
        // recovery protocol. Start it only after the Wi-Fi bootstrap has finished;
        // otherwise both tasks can consume different halves of the same VLW1 frame.
        auto connection = BootstrapLinxOtaConfig(assembly_->board_identity(),
                                                 [this](std::string_view title, std::string_view detail) {
                                                     ShowDisplay(voice::VoiceMood::kConnecting, title, detail);
                                                 });
        // Bootstrap 无论是下发连接配置还是返回“待控制台激活”，均可能已经
        // 完成 STA 关联。由 Runtime 把受控网络事实写入快照，Renderer 只显示
        // 语义而不触碰 ESP Wi-Fi API。
        EnqueueNetworkState(LinxWifiStaConnected());
        if (!connection.ok() || !connection.value.has_value()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=linx_bootstrap code=%d", static_cast<int>(connection.status.code));
            ShowDisplay(voice::VoiceMood::kSad, "错误", "");
            return fail_startup(connection.status);
        }
        ShowDisplay(voice::VoiceMood::kConnecting, "连接", "");
        linx_config_ = std::move(*connection.value);
        // SparkBot keeps I2S/VAD/serial injection at PCM 16 kHz / 20 ms.
        // The Linx provider converts only at the WebSocket boundary, matching
        // the platform's documented low-latency Opus VoIP profile.
        linx_config_.preferred_audio = {.codec = voice::AudioCodec::kOpus,
                                        .sample_rate_hz = 16000,
                                        .channels = 1,
                                        .bits_per_sample = 16,
                                        .frame_duration_ms = 20};
#if CONFIG_VOICELIFE_IM_GATEWAY
        // Start IM provisioning after Linx has released the shared USB console.
        if (!StartImProvisioningTask()) {
            ESP_LOGW(kTag, "IM_PROVISION_TASK_FAILED=1");
        }
#endif
        auto result = registry.Create("xrobot-websocket", {});
#else
        auto result = registry.Create("scaffold", {});
#endif
        if (!result.ok() || !result.value.has_value()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=provider_create code=%d", static_cast<int>(result.status.code));
            return fail_startup(Status::Error(ErrorCode::kInternal, "无法创建语音 Provider: " + result.status.message));
        }
        provider_ = std::move(*result.value);

#ifdef ESP_PLATFORM
        // 音频端口由 Assembly 注入（业务 PCM 语义，不暴露 I2S/Codec）。
        if (assembly_->uses_local_wake_detector()) {
            assembly_->wake_gate().SetWakeSink([this](std::string_view wake_word) { QueueWakeWord(wake_word); });
        }
        session_ = std::make_unique<voice::VoiceSession>(
            assembly_->wake_gate(), assembly_->audio_output(), *provider_,
            [this](const voice::VoiceEvidence& evidence) { LogVoiceEvidence(evidence); });
#else
        session_ = std::make_unique<voice::VoiceSession>(audio_input_, audio_output_, *provider_);
#endif
        const Status session_status = session_->Start(config);
        if (!session_status.ok()) {
            ESP_LOGW(kTag, "STARTUP_ERROR stage=session_start code=%d", static_cast<int>(session_status.code));
            ShowDisplay(voice::VoiceMood::kSad, "错误", "");
            return fail_startup(session_status);
        }

#ifdef ESP_PLATFORM
        // IM 的 SNTP、Gateway 探针和退避全部在独立任务中完成，语音启动路径不等待网络。
        StartImRuntime();
        EnqueueEvent(voice::VoiceInteractionEvent::kBootCompleted);
        const Status input_status =
            assembly_->StartBoardInput([this](BoardInputAction action) { (void)EnqueueBoardInput(action); });
        if (!input_status.ok()) return fail_startup(input_status);
#if CONFIG_VOICELIFE_SERIAL_VOICE_TEST
        if (const Status serial_test_status = StartSerialVoiceTest(); !serial_test_status.ok()) {
            return fail_startup(serial_test_status);
        }
#endif
#if CONFIG_VOICELIFE_STATE_FLOW_TEST
        if (const Status state_flow_status = StartStateFlowDiagnostic(); !state_flow_status.ok()) {
            return fail_startup(state_flow_status);
        }
#endif
#endif
        return Status::Ok();
    }

   private:
    StorageBootstrap storage_;
#ifdef ESP_PLATFORM
    void StopEventLoop() {
        if (event_task_ == nullptr) return;
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            control_event_queue_.clear();
            best_effort_event_queue_.clear();
            event_loop_stop_ = true;
        }
        event_cv_.notify_one();
        for (int attempt = 0; attempt < 20 && !event_loop_stopped_; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        event_task_ = nullptr;
    }

    void StopScheduleReminderRuntime() {
        if (!StopReminderActionWorker()) {
            // 动作流仍可能持有 ReminderService；在 worker 真正退出前禁止释放其依赖，
            // 否则启动失败清理路径也会把未完成的 SSE 回调变成悬空引用。
            ESP_LOGE(kTag, "IM_ACTION_WORKER_STOP_TIMEOUT=1 dependencies_kept=1");
            return;
        }
        if (schedule_reminder_service_) {
            schedule_reminder_service_->Stop();
        }
        reminder_action_executor_.reset();
        schedule_reminder_service_.reset();
        reminder_notification_.reset();
        reminder_speech_.reset();
        timing_runtime_.reset();
    }

    struct McpRequest {
        std::string payload;
        std::string session_id;
        std::mutex mutex;
        std::condition_variable completed_cv;
        std::optional<Result<std::string>> response;
        bool completed = false;
        std::atomic_bool abandoned{false};
    };

    static constexpr std::size_t kMcpWorkerQueueCapacity = 4;

    Status StartMcpWorker() {
        std::lock_guard<std::mutex> lock(mcp_mutex_);
        if (mcp_task_ != nullptr) {
            // 旧 worker 可能仍在执行网络请求；未确认退出前不得重建，避免双 worker
            // 并发访问队列、MCP server 与 BindingUseCase。
            if (!mcp_stopped_.load()) {
                return Status::Error(ErrorCode::kInternal, "MCP 工作任务尚未退出");
            }
            mcp_task_ = nullptr;  // 任务已自删，仅句柄残留。
        }
        mcp_stop_ = false;
        mcp_stopped_.store(false);
        if (xTaskCreate(&Runtime::McpWorkerTaskEntry, "voicelife_mcp", 32768, this, 4, &mcp_task_) != pdPASS) {
            return Status::Error(ErrorCode::kInternal, "创建 MCP 工作任务失败");
        }
        ESP_LOGI(kTag, "MCP_WORKER_READY capacity=%u", static_cast<unsigned>(kMcpWorkerQueueCapacity));
        return Status::Ok();
    }

    void StopMcpWorker() {
        {
            std::lock_guard<std::mutex> lock(mcp_mutex_);
            if (mcp_task_ == nullptr) return;
            mcp_stop_ = true;
            for (const auto& request : mcp_queue_) request->abandoned.store(true);
            mcp_queue_.clear();
        }
        mcp_cv_.notify_all();
        // 有界等待任务确认退出。worker 内 HTTPS 请求最长约 10s（传输层超时），
        // 等待上限给足 5s；仍未退出时保留句柄并报错，拒绝在旧任务存续期重建。
        constexpr int kStopWaitAttempts = 500;
        for (int attempt = 0; attempt < kStopWaitAttempts && !mcp_stopped_.load(); ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (mcp_stopped_.load()) {
            std::lock_guard<std::mutex> lock(mcp_mutex_);
            mcp_task_ = nullptr;
        } else {
            ESP_LOGE(kTag, "MCP_WORKER_STOP_TIMEOUT=1 task_still_running=1");
        }
    }

    // ---- im.binding.start 有界后台轮询 ----
    static constexpr uint32_t kBindingPollIntervalMs = 3000;
    // Poll 内含 HTTPS 查询与 JSON 解析，但无 MCP/Linx 调用链；栈按 24KB 预留，
    // 需以真机 uxTaskGetStackHighWaterMark 实测校准（任务退出时已上报高水位）。
    // 交互期间内部 RAM 最大连续块可低于 16KB，栈必须分配到已启用的 PSRAM；
    // TCB 仍由 FreeRTOS 放在内部 RAM。WithCaps 创建的任务须配对 WithCaps 删除。
    static constexpr uint32_t kBindingPollStackBytes = 24 * 1024;

    void StartBindingPolling(uint64_t generation) {
        if (!binding_poll_lease_.Acquire(generation)) {
            ESP_LOGI(kTag, "IM_BINDING_POLL_ADOPTED generation=%llu", static_cast<unsigned long long>(generation));
            return;
        }
        if (xTaskCreateWithCaps(&Runtime::BindingPollTaskEntry, "voicelife_binding_poll", kBindingPollStackBytes, this,
                                2, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            if (binding_poll_lease_.Release(generation)) {
                EnqueueBindingResult(binding_use_case_.AbortPending(generation));
            }
            ESP_LOGW(kTag, "IM_BINDING_POLL_TASK_FAILED=1");
            return;
        }
        ESP_LOGI(kTag, "IM_BINDING_POLL_STARTED generation=%llu", static_cast<unsigned long long>(generation));
    }

    static void BindingPollTaskEntry(void* context) { static_cast<Runtime*>(context)->BindingPollLoop(); }

    void BindingPollLoop() {
        while (true) {
            const uint64_t owner_generation = binding_poll_lease_.generation();
            vTaskDelay(pdMS_TO_TICKS(kBindingPollIntervalMs));
            const im::BindingResult result = binding_use_case_.Poll();
            if (result.state == im::BindingState::kPending || result.state == im::BindingState::kWaiting ||
                result.state == im::BindingState::kRetrying) {
                continue;
            }
            // 轮询任务只投递脱敏语义结果。事件循环按 BindingUseCase generation
            // 丢弃 origin/凭据变更后迟到的旧 confirmed，绝不直接访问显示或语音硬件。
            EnqueueBindingResult(result);
            // 终态或会话已释放。若新 Start 在旧任务退出窗口接管租约，Release
            // 会失败，本任务继续服务新会话，避免出现 pending 却没有轮询任务。
            if (binding_use_case_.active()) continue;
            if (binding_poll_lease_.Release(owner_generation)) {
                ESP_LOGI(kTag, "IM_BINDING_STATUS=%s stack_high_water=%u", BindingStatusName(result.state),
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                break;
            }
        }
        ESP_LOGI(kTag, "IM_BINDING_POLL_STOPPED=1");
        vTaskDeleteWithCaps(nullptr);
    }

    static std::string TruncateUtf8(std::string_view value, std::size_t max_bytes) {
        if (value.size() <= max_bytes) return std::string(value);
        std::size_t end = max_bytes;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) --end;
        return std::string(value.substr(0, end)) + "...";
    }

    static bool IsMcpToolCall(std::string_view payload) {
        JsonValue request;
        if (!ParseJson(payload, request).ok() || !request.IsObject()) return false;
        const JsonValue* method = request.Get("method");
        return method != nullptr && method->IsString() && method->string == "tools/call";
    }

    static std::string McpToolName(std::string_view payload) {
        JsonValue request;
        if (!ParseJson(payload, request).ok() || !request.IsObject()) return "invalid";
        const JsonValue* params = request.Get("params");
        const JsonValue* name = params != nullptr && params->IsObject() ? params->Get("name") : nullptr;
        return name != nullptr && name->IsString() ? name->string : "missing";
    }

    static std::string McpMethod(std::string_view payload) {
        JsonValue request;
        if (!ParseJson(payload, request).ok() || !request.IsObject()) return "invalid";
        const JsonValue* method = request.Get("method");
        return method != nullptr && method->IsString() ? method->string : "missing";
    }

    static std::string McpRequestId(std::string_view payload) {
        JsonValue request;
        if (!ParseJson(payload, request).ok() || !request.IsObject()) return "invalid";
        const JsonValue* id = request.Get("id");
        if (id == nullptr) return "notification";
        if (id->IsString()) return id->string;
        if (id->kind == JsonValue::Kind::kNumber) return std::to_string(static_cast<int64_t>(id->number));
        return "non_scalar";
    }

    Result<std::string> HandleMcpRequest(std::string_view payload, std::string_view session_id) {
        const std::string method = McpMethod(payload);
        const std::string request_id = McpRequestId(payload);
        ESP_LOGI(kTag, "MCP_RX method=%s id=%s bytes=%u session_len=%u", method.c_str(), request_id.c_str(),
                 static_cast<unsigned>(payload.size()), static_cast<unsigned>(session_id.size()));
        auto request = std::make_shared<McpRequest>();
        request->payload.assign(payload);
        request->session_id.assign(session_id);
        {
            std::lock_guard<std::mutex> lock(mcp_mutex_);
            if (mcp_stop_ || mcp_task_ == nullptr || mcp_queue_.size() >= kMcpWorkerQueueCapacity) {
                ESP_LOGW(kTag, "MCP_REQUEST_REJECTED reason=queue_full");
                return BuildLinxMcpUnavailableResponse(payload, "设备 MCP 正忙，请稍后重试", session_id);
            }
            mcp_queue_.push_back(request);
        }
        ESP_LOGI(kTag, "MCP_REQUEST_QUEUED method=%s id=%s bytes=%u", method.c_str(), request_id.c_str(),
                 static_cast<unsigned>(payload.size()));
        mcp_cv_.notify_one();

        std::unique_lock<std::mutex> lock(request->mutex);
        if (!request->completed_cv.wait_for(lock, std::chrono::milliseconds(kMcpResponseTimeoutMs),
                                            [&] { return request->completed; })) {
            request->abandoned.store(true);
            ESP_LOGW(kTag, "MCP_REQUEST_REJECTED reason=timeout");
            return BuildLinxMcpUnavailableResponse(payload, "设备 MCP 响应超时", session_id);
        }
        return std::move(*request->response);
    }

    static void McpWorkerTaskEntry(void* arg) { static_cast<Runtime*>(arg)->McpWorkerLoop(); }

    void McpWorkerLoop() {
        while (true) {
            std::shared_ptr<McpRequest> request;
            {
                std::unique_lock<std::mutex> lock(mcp_mutex_);
                mcp_cv_.wait(lock, [this] { return mcp_stop_ || !mcp_queue_.empty(); });
                if (mcp_stop_ && mcp_queue_.empty()) break;
                request = std::move(mcp_queue_.front());
                mcp_queue_.pop_front();
            }
            if (request->abandoned.load()) continue;
            const bool tool_call = IsMcpToolCall(request->payload);
            const std::string tool_name = tool_call ? McpToolName(request->payload) : std::string{};
            if (tool_call && session_) session_->ReportToolCallStarted();
            LinxMcpToolOutcome outcome;
            auto response = HandleLinxMcpPayload(request->payload, mcp_server_, request->session_id,
                                                 tool_call ? &outcome : nullptr);
            if (!response.ok()) {
                response = BuildLinxMcpUnavailableResponse(request->payload, "设备 MCP 执行失败", request->session_id);
                outcome = InspectLinxMcpToolOutcome(request->payload, response);
            }
            if (tool_call && !request->abandoned.load() && session_) {
                session_->ReportToolResult(TruncateUtf8(outcome.summary, 96), outcome.success);
            }
            ESP_LOGI(kTag, "MCP_TX method=%s id=%s bytes=%u result=%d", McpMethod(request->payload).c_str(),
                     McpRequestId(request->payload).c_str(),
                     response.ok() && response.value.has_value() ? static_cast<unsigned>(response.value->size()) : 0U,
                     response.ok() ? 1 : 0);
            ESP_LOGI(kTag, "MCP_TOOL_EXECUTED tool_call=%d result=%d", tool_call ? 1 : 0, response.ok() ? 1 : 0);
            if (tool_call) {
                ESP_LOGI(kTag, "MCP_TOOL_OUTCOME name=%s result_status=%s success=%d", tool_name.c_str(),
                         outcome.result_status.c_str(), outcome.success ? 1 : 0);
            }
            {
                std::lock_guard<std::mutex> lock(request->mutex);
                if (!request->abandoned.load()) {
                    request->response = std::move(response);
                    request->completed = true;
                }
            }
            request->completed_cv.notify_one();
        }
        mcp_stopped_.store(true);
        vTaskDelete(nullptr);
    }

    void ReserveImRuntimeTask() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        bool expected = false;
        if (!im_lifecycle_reserved_.compare_exchange_strong(expected, true)) return;
        // This task loads IM configuration from encrypted NVS. NVS can disable
        // flash cache, so ESP-IDF requires the caller's stack to be in DRAM.
        if (xTaskCreate(&Runtime::ImLifecycleTaskEntry, "voicelife_im_lifecycle", 8192, this, 3, &im_lifecycle_task_) !=
            pdPASS) {
            im_lifecycle_reserved_.store(false);
            ESP_LOGW(kTag, "IM_RUNTIME_TASK_FAILED=1");
            return;
        }
        ESP_LOGI(kTag, "IM_RUNTIME_TASK_RESERVED=1");
#else
        ESP_LOGI(kTag, "IM_RUNTIME_DISABLED=1");
#endif
    }

    void StartImRuntime() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        ReserveImRuntimeTask();
        if (im_lifecycle_task_ == nullptr) return;
        bool expected = false;
        if (!im_lifecycle_activated_.compare_exchange_strong(expected, true)) return;
        xTaskNotifyGive(im_lifecycle_task_);
#else
        ESP_LOGI(kTag, "IM_RUNTIME_DISABLED=1");
#endif
    }

    void EnqueueReminderActionWindow(im::ActionWindow window) {
#if CONFIG_VOICELIFE_IM_GATEWAY
        ESP_LOGI(kTag, "IM_ACTION_WINDOW_ENQUEUED=1 reminder_trigger_id=%s expires_at=%s",
                 window.reminderTriggerId.c_str(), window.expiresAt.c_str());
        {
            std::lock_guard<std::mutex> lock(im_action_mutex_);
            im_action_windows_.push_back(std::move(window));
        }
        if (im_runtime_.state() == im::ImRuntimeState::kReady) DrainReminderActionWindows();
#else
        (void)window;
#endif
    }

    void DrainReminderActionWindows() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        // The single worker is reserved during startup; action windows only need
        // to wake its bounded polling loop after they are queued.
        (void)im_action_worker_task_;
#endif
    }

    void ReportPersistedVoiceActions() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        if (!schedule_reminder_service_ || im_runtime_.state() != im::ImRuntimeState::kReady ||
            im_runtime_.reporting_channel() == nullptr || im_runtime_.device_id().empty())
            return;
        const auto persisted = schedule_reminder_service_->ListPersistedVoiceActionResults();
        if (!persisted.ok()) return;
        for (const auto& result : *persisted.value) {
            contracts::im::ReminderActionStatusReport report;
            report.schemaVersion = contracts::im::kDeviceContractVersion;
            report.eventId = "voice-action:" + result.operation_id;
            report.correlationId = result.operation_id;
            report.deviceId = im_runtime_.device_id();
            report.reminderTriggerId = result.reminder_trigger_id;
            report.operationId = result.operation_id;
            report.action = result.action == schedule::ScheduleReminderActionKind::kSnooze ? "snooze" : "acknowledge";
            report.status = "succeeded";
            report.occurredAt = FormatReminderIso(result.occurred_at);
            if (result.next_trigger_at.has_value()) report.nextTriggerAt = FormatReminderIso(*result.next_trigger_at);
            report.source = "voice";
            const auto submitted = im_runtime_.reporting_channel()->SubmitReminderActionStatusReport(report);
            ESP_LOGI(kTag, "IM_VOICE_ACTION_REPORT event_id=%s status=%d trigger=%s", report.eventId.c_str(),
                     static_cast<int>(submitted.status), report.reminderTriggerId.c_str());
        }
#endif
    }

    bool StartReminderActionWorker() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        if (im_action_worker_task_ != nullptr) return true;
        constexpr uint32_t kReminderActionWorkerStackBytes = 16 * 1024;
        im_action_stop_.store(false);
        im_action_worker_stopped_.store(false);
        im_action_worker_count_.store(1);
        if (xTaskCreateWithCaps(&Runtime::ReminderActionTaskEntry, "voicelife_im_actions",
                                kReminderActionWorkerStackBytes, this, 3, &im_action_worker_task_,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            im_action_worker_task_ = nullptr;
            im_action_worker_count_.store(0);
            im_action_worker_stopped_.store(true);
            ESP_LOGW(kTag, "IM_ACTION_TASK_FAILED=1");
            return false;
        }
        ESP_LOGI(kTag, "IM_ACTION_WORKER_READY stack_bytes=%u caps=internal",
                 static_cast<unsigned>(kReminderActionWorkerStackBytes));
        return true;
#else
        return true;
#endif
    }

    static void ReminderActionTaskEntry(void* context) { static_cast<Runtime*>(context)->ReminderActionTask(); }

    void ReminderActionTask() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        int64_t last_report_us = 0;
        while (!im_action_stop_.load()) {
            im::ActionWindow window;
            bool has_window = false;
            {
                std::lock_guard<std::mutex> lock(im_action_mutex_);
                if (!im_action_windows_.empty()) {
                    window = std::move(im_action_windows_.front());
                    im_action_windows_.pop_front();
                    has_window = true;
                }
            }
            if (!has_window) {
                const int64_t now_us = esp_timer_get_time();
                if (now_us - last_report_us >= kVoiceActionReportRetryUs) {
                    ReportPersistedVoiceActions();
                    last_report_us = now_us;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            im::ImActionChannel channel(*im_runtime_.reporting_channel(), im_config_, *reminder_action_executor_,
                                        reminder_action_clock_);
            while (!im_action_stop_.load()) {
                if (im_runtime_.state() != im::ImRuntimeState::kReady) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                im::EspActionStreamTransport stream(im_gateway_origin_, im_config_, window.reminderTriggerId);
                const im::ActionRunResult result = channel.Run(stream, window);
                ESP_LOGI(kTag,
                         "IM_ACTION_STREAM_RESULT status=%d executed=%d confirmed=%d dropped=%d reminder_trigger_id=%s",
                         static_cast<int>(result.status), result.executed, result.confirmed, result.dropped,
                         window.reminderTriggerId.c_str());
                if (result.status != im::ActionRunStatus::kDisconnected) break;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
#endif
#if CONFIG_VOICELIFE_IM_GATEWAY
        im_action_worker_count_.fetch_sub(1);
        if (im_action_worker_count_.load() == 0) im_action_worker_stopped_.store(true);
        im_action_worker_task_ = nullptr;
#endif
        vTaskDeleteWithCaps(nullptr);
    }

    bool StopReminderActionWorker() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        im_action_stop_.store(true);
        if (im_action_worker_count_.load() == 0) {
            im_action_worker_stopped_.store(true);
        }
        // EspActionStreamTransport may be blocked in one bounded 30s HTTP read;
        // wait longer than that bound instead of destroying its dependencies while
        // the worker is still inside ImActionChannel::Run().
        constexpr int kStopWaitAttempts = 4000;
        for (int attempt = 0; attempt < kStopWaitAttempts && im_action_worker_count_.load() != 0; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (im_action_worker_count_.load() != 0) {
            return false;
        }
        im_action_worker_task_ = nullptr;
        im_action_worker_stopped_.store(true);
        return true;
#else
        return true;
#endif
    }

    void ResumeReminderActionWorker() {
#if CONFIG_VOICELIFE_IM_GATEWAY
        im_action_stop_.store(false);
        im_action_worker_stopped_.store(false);
        if (im_runtime_.state() == im::ImRuntimeState::kReady) DrainReminderActionWindows();
#endif
    }

    static void ImLifecycleTaskEntry(void* context) {
        auto* runtime = static_cast<Runtime*>(context);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!runtime->im_lifecycle_activated_.load()) {
            runtime->im_lifecycle_task_ = nullptr;
            runtime->im_lifecycle_reserved_.store(false);
            vTaskDelete(nullptr);
            return;
        }
        runtime->ImLifecycleTask();
    }

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
            if (im_readiness_.SystemTimeReady() && !im_system_time_logged_) {
                ESP_LOGI(kTag, "SNTP_SYNCED=1");
                im_system_time_logged_ = true;
            }
            status = im_runtime_.Start();
            if (im_runtime_.state() == im::ImRuntimeState::kProbing) {
                response = im_runtime_.ProbeGateway();
                if (im_runtime_.state() != im::ImRuntimeState::kReady) {
                    status = Status::Error(ErrorCode::kUnavailable, "IM Gateway 认证探针失败");
                }
            }

            if (im_runtime_.state() == im::ImRuntimeState::kReady) {
                const auto action_config = im_config_.Load();
                if (action_config.ok()) im_gateway_origin_ = action_config.value->gateway_origin;
                // 选择 #235 的“重启后重新开始”策略：不恢复任何旧会话；下一次
                // 明确语音命令会创建新会话，Gateway 会原子取消同设备旧 pending。
                binding_use_case_.Bind(*im_runtime_.pairing_client(), im_pairing_clock_, im_runtime_.user_id());
                EnqueueBindingReset(binding_use_case_.generation());
                RegisterImPairingAcceptance(im_runtime_.pairing_client(), im_runtime_.device_id(),
                                            im_runtime_.user_id());
                ResumeReminderActionWorker();
                ReportPersistedVoiceActions();
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
        im_lifecycle_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    enum class BoardRequestKind : uint8_t {
        kWakeWord,
        kInterruptAndWakeWord,
        kRestoreStandby,
        kInterrupt,
        kStartCapture,
        kStopCapture,
        kInterruptAndStartCapture,
    };

    struct BoardRequest {
        BoardRequestKind kind = BoardRequestKind::kRestoreStandby;
        char wake_word[32];
        /** 物理唤醒门已就绪后是否需将 Controller 收口为 standby。 */
        bool settle_controller = true;
        /** 请求创建时的控制器状态；过期请求不得覆盖新的语音回合。 */
        voice::VoiceInteractionState expected_state = voice::VoiceInteractionState::kBooting;
        /** 当存在时，以 Provider 的正式 TTS 请求播报这段系统话术。 */
        char system_speech[kBindingSystemSpeechCapacity];
    };

    bool EnqueueBoardInput(BoardInputAction action) {
        InteractionEventItem item{};
        item.board_input = true;
        item.board_action = action;
        return EnqueueInteractionItem(item);
    }

#if CONFIG_VOICELIFE_SERIAL_VOICE_TEST
    Status StartSerialVoiceTest() {
        if (assembly_ == nullptr || assembly_->test_audio_injection() == nullptr) {
            return Status::Error(ErrorCode::kUnavailable, "当前板型不支持串口语音测试注入");
        }
        SerialVoiceTestCallbacks callbacks;
        callbacks.begin_turn = [this]() {
            auto* injection = assembly_ != nullptr ? assembly_->test_audio_injection() : nullptr;
            if (injection == nullptr) return Status::Error(ErrorCode::kUnavailable, "测试注入端口不可用");
            const Status enabled = injection->SetTestInputEnabled(true);
            if (!enabled.ok()) return enabled;
            // TTS 结束后 Runtime 会自动打开下一轮采集；串口夹具仍需显式
            // 重新声明注入窗口，但不能再次投递 PressDown 破坏 Listening 状态。
            if (interaction_.state() == voice::VoiceInteractionState::kListening) return Status::Ok();
            if (EnqueueBoardInput(BoardInputAction::kPressDown)) return Status::Ok();
            (void)injection->SetTestInputEnabled(false);
            return Status::Error(ErrorCode::kUnavailable, "语音测试开始事件未进入状态机队列");
        };
        callbacks.submit_pcm = [this](voice::AudioFrame frame) {
            auto* injection = assembly_ != nullptr ? assembly_->test_audio_injection() : nullptr;
            return injection != nullptr ? injection->InjectTestInput(std::move(frame))
                                        : Status::Error(ErrorCode::kUnavailable, "测试注入端口不可用");
        };
        callbacks.end_turn = [this]() {
            auto* injection = assembly_ != nullptr ? assembly_->test_audio_injection() : nullptr;
            if (injection == nullptr) return Status::Error(ErrorCode::kUnavailable, "测试注入端口不可用");
            const Status disabled = injection->SetTestInputEnabled(false);
            if (!disabled.ok()) return disabled;
            return EnqueueBoardInput(BoardInputAction::kPressUp)
                       ? Status::Ok()
                       : Status::Error(ErrorCode::kUnavailable, "语音测试结束事件未进入状态机队列");
        };
        callbacks.begin_wake = [this]() {
            auto* injection = assembly_ != nullptr ? assembly_->test_audio_injection() : nullptr;
            if (injection == nullptr) return Status::Error(ErrorCode::kUnavailable, "测试注入端口不可用");
            if (!assembly_->wake_gate().standby()) {
                return Status::Error(ErrorCode::kConflict, "本地唤醒注入要求设备处于待机");
            }
            return injection->SetTestInputEnabled(true);
        };
        callbacks.end_wake = [this]() {
            auto* injection = assembly_ != nullptr ? assembly_->test_audio_injection() : nullptr;
            return injection != nullptr ? injection->SetTestInputEnabled(false)
                                        : Status::Error(ErrorCode::kUnavailable, "测试注入端口不可用");
        };
        serial_voice_test_ = std::make_unique<SerialVoiceTest>(std::move(callbacks));
        return serial_voice_test_->Start();
    }
#endif

    void SetVolume(int volume) {
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

    void QueueWakeWord(std::string_view wake_word) {
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

    bool EnqueueBoardRequest(const BoardRequest& request, std::string_view action) {
        if (wake_queue_ == nullptr) {
            ESP_LOGE(kTag, "BOARD_REQUEST_UNAVAILABLE action=%.*s", static_cast<int>(action.size()), action.data());
            return false;
        }
        // 仅由事件循环或 WakeTask 调用，绝不在 I2S、VAD 或网络回调上等待。
        // 短暂等待可覆盖 WakeTask 正在收尾前一请求的窗口；超时必须显式上报，
        // 不能让 UI 已迁移而硬件动作被静默丢弃。
        constexpr TickType_t kBoardRequestWait = pdMS_TO_TICKS(50);
        if (xQueueSend(wake_queue_, &request, kBoardRequestWait) == pdTRUE) return true;
        const uint32_t dropped = ++dropped_board_requests_;
        ESP_LOGE(kTag, "BOARD_REQUEST_QUEUE_FULL action=%.*s dropped=%u", static_cast<int>(action.size()),
                 action.data(), static_cast<unsigned>(dropped));
        return false;
    }

    bool QueueVoiceTurn(std::string_view wake_word) {
        BoardRequest request{};
        request.kind = BoardRequestKind::kWakeWord;
        const std::size_t size =
            wake_word.size() < sizeof(request.wake_word) - 1 ? wake_word.size() : sizeof(request.wake_word) - 1;
        std::memcpy(request.wake_word, wake_word.data(), size);
        request.wake_word[size] = '\0';
        return EnqueueBoardRequest(request, "wake_word");
    }

    bool QueueInterruptAndVoiceTurn(std::string_view wake_word) {
        BoardRequest request{};
        request.kind = BoardRequestKind::kInterruptAndWakeWord;
        const std::size_t size =
            wake_word.size() < sizeof(request.wake_word) - 1 ? wake_word.size() : sizeof(request.wake_word) - 1;
        std::memcpy(request.wake_word, wake_word.data(), size);
        request.wake_word[size] = '\0';
        return EnqueueBoardRequest(request, "interrupt_wake_word");
    }

    bool QueueStandbyRecovery(bool settle_controller = true) {
        BoardRequest recovery{};
        recovery.settle_controller = settle_controller;
        recovery.expected_state = interaction_.state();
        return EnqueueBoardRequest(recovery, "restore_standby");
    }

    bool QueueSystemSpeech(std::string_view text) {
        if (wake_queue_ == nullptr || text.empty()) return false;
        if (text.size() >= kBindingSystemSpeechCapacity) {
            ESP_LOGE(kTag, "SYSTEM_SPEECH_TOO_LONG bytes=%u", static_cast<unsigned>(text.size()));
            return false;
        }
        BoardRequest request{};
        request.kind = BoardRequestKind::kInterrupt;
        std::memcpy(request.system_speech, text.data(), text.size());
        request.system_speech[text.size()] = '\0';
        // 先让交互单写者离开待机，再由 WakeTask 取消旧回合并提交 TTS。
        // 这样提醒的 tts.started/字幕不会在 Standby 门控中被丢弃。
        if (!EnqueueEvent(voice::VoiceInteractionEvent::kSystemSpeechRequested)) return false;
        if (EnqueueBoardRequest(request, "system_speech")) return true;
        (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
        return false;
    }

    // 下行长文本滚动由显示 Adapter 负责（Ssd1306PresentationAdapter）。
    // 音量 overlay 到期：递增 revision 触发 CommitSnapshot 恢复最新快照。
    static void VolumeOverlayEntry(void* context) {
        auto* self = static_cast<Runtime*>(context);
        self->volume_overlay_until_us_ = 0;
        self->overlay_expired_.store(true);  // 只置标志；恢复由事件循环唯一执行。
    }

    // 聆听/最终 STT 超时：
    // - kListening 超时（无有效输入）：结束本轮回待机
    // - kFinalizing 超时（listen.stop 后 5s 无最终 STT）：abort 结束服务端回合回待机
    static void ListenTimeoutEntry(void* context) {
        auto* self = static_cast<Runtime*>(context);
        // Timer 回调不能读取或迁移交互状态；由事件循环串行决定超时路径。
        ESP_LOGI(kTag, "LISTEN_TIMEOUT_FIRED");
        self->EnqueueListenTimeout();
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
        const esp_err_t start = esp_timer_start_once(listen_timer_, timeout_ms * 1000ULL);
        if (start != ESP_OK) {
            ESP_LOGW(kTag, "LISTEN_TIMEOUT_ARM_FAILED ms=%u err=%d", static_cast<unsigned>(timeout_ms),
                     static_cast<int>(start));
            return;
        }
        ESP_LOGI(kTag, "LISTEN_TIMEOUT_ARMED ms=%u", static_cast<unsigned>(timeout_ms));
    }

    void CancelListenTimer() {
        if (listen_timer_ != nullptr) {
            (void)esp_timer_stop(listen_timer_);
        }
    }

    bool QueueInterrupt() {
        BoardRequest request{};
        request.kind = BoardRequestKind::kInterrupt;
        return EnqueueBoardRequest(request, "interrupt");
    }

    bool QueueCaptureStart() {
        BoardRequest request{};
        request.kind = BoardRequestKind::kStartCapture;
        return EnqueueBoardRequest(request, "start_capture");
    }

    bool QueueCaptureStop() {
        BoardRequest request{};
        request.kind = BoardRequestKind::kStopCapture;
        request.expected_state = interaction_.state();
        return EnqueueBoardRequest(request, "stop_capture");
    }

    bool QueueInterruptAndCapture() {
        BoardRequest request{};
        request.kind = BoardRequestKind::kInterruptAndStartCapture;
        return EnqueueBoardRequest(request, "interrupt_start_capture");
    }

#if CONFIG_VOICELIFE_STATE_FLOW_TEST
    Status StartStateFlowDiagnostic() {
        if (state_flow_task_ != nullptr) return Status::Ok();
        if (xTaskCreate(&Runtime::StateFlowTaskEntry, "voicelife_state_flow", 4096, this, 1, &state_flow_task_) !=
            pdPASS) {
            return Status::Error(ErrorCode::kInternal, "创建状态流诊断任务失败");
        }
        ESP_LOGI(kTag, "STATE_FLOW_TEST_STARTED production_default=0");
        return Status::Ok();
    }

    static void StateFlowTaskEntry(void* context) { static_cast<Runtime*>(context)->StateFlowTask(); }

    void StateFlowEvent(uint32_t step, voice::VoiceInteractionEvent event) {
        ESP_LOGI(kTag, "STATE_FLOW_ENQUEUE step=%u kind=interaction event=%d", static_cast<unsigned>(step),
                 static_cast<int>(event));
        EnqueueEvent(event);
    }

    void StateFlowEvidence(uint32_t step, std::string_view event, std::string_view detail = {}) {
        ESP_LOGI(kTag, "STATE_FLOW_ENQUEUE step=%u kind=evidence event=%.*s detail_bytes=%u",
                 static_cast<unsigned>(step), static_cast<int>(event.size()), event.data(),
                 static_cast<unsigned>(detail.size()));
        voice::VoiceEvidence evidence;
        evidence.session_id = session_ ? session_->config().session_id : "state-flow";
        evidence.generation = session_ ? session_->generation() : 0;
        evidence.event = std::string(event);
        evidence.detail = std::string(detail);
        EnqueueVoiceEvidence(evidence);
    }

    void StateFlowTask() {
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

    void RestoreStandby(const BoardRequest& request) {
        if (assembly_ == nullptr) return;
        const auto actual_state = interaction_.state();
        if (actual_state != request.expected_state) {
            // The interaction loop owns the state machine while WakeTask owns
            // hardware. A queued recovery must not stop a newer capture or
            // put its obsolete kStandbyReady back into the event loop.
            ESP_LOGW(kTag, "BOARD_REQUEST_STALE kind=restore_standby expected_state=%d actual_state=%d",
                     static_cast<int>(request.expected_state), static_cast<int>(actual_state));
            return;
        }
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

    /** @brief WakeTask 是板级请求的唯一消费者，恢复时不可向自身队列回投。 */
    void RestoreStandbyFromWakeTask(bool settle_controller = true) {
        BoardRequest request{};
        request.kind = BoardRequestKind::kRestoreStandby;
        request.settle_controller = settle_controller;
        request.expected_state = interaction_.state();
        RestoreStandby(request);
    }

    static void WakeTaskEntry(void* context) { static_cast<Runtime*>(context)->WakeTask(); }

    void WakeTask() {
        BoardRequest request{};
        while (true) {
            if (xQueueReceive(wake_queue_, &request, portMAX_DELAY) != pdTRUE) continue;
            if (request.kind == BoardRequestKind::kRestoreStandby) {
                RestoreStandby(request);
                continue;
            }
            if (request.kind == BoardRequestKind::kInterruptAndWakeWord) {
                if (!session_ || !provider_) {
                    ESP_LOGW(kTag, "打断确认请求不可用");
                    RestoreStandbyFromWakeTask();
                    continue;
                }
                const Status acknowledge = session_->InterruptAndNotifyLocalWakeWord(request.wake_word, "收到！");
                if (!acknowledge.ok()) {
                    ESP_LOGW(kTag, "打断确认请求失败: %s", acknowledge.message.c_str());
                    RestoreStandbyFromWakeTask();
                }
                continue;
            }
            if (request.kind == BoardRequestKind::kInterrupt) {
                if (!session_) {
                    ESP_LOGW(kTag, "板端打断不可用");
                    RestoreStandbyFromWakeTask();
                    continue;
                }
                const Status interrupt = session_->Interrupt();
                if (request.system_speech[0] != '\0') {
                    const Status speak = interrupt.ok() ? session_->Speak(request.system_speech) : interrupt;
                    if (!speak.ok()) {
                        ESP_LOGW(kTag, "系统播报请求失败: %s", speak.message.c_str());
                        RestoreStandbyFromWakeTask();
                    }
                    continue;
                }
                if (interrupt.ok()) {
                    if (interaction_.state() == voice::VoiceInteractionState::kInterrupting) {
                        (void)EnqueueEvent(voice::VoiceInteractionEvent::kInterruptCompleted);
                    } else {
                        RestoreStandbyFromWakeTask();
                    }
                } else {
                    ESP_LOGW(kTag, "板端打断失败: %s", interrupt.message.c_str());
                    RestoreStandbyFromWakeTask();
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
                const auto actual_state = interaction_.state();
                if (actual_state != request.expected_state) {
                    // 服务端 VAD/TTS 可能在 WakeTask 消费排队 stop 前完成本轮。
                    // 不能让旧 stop 停掉新阶段，或把已开始的播报升级为失败。
                    ESP_LOGI(kTag, "BOARD_REQUEST_STALE kind=stop_capture expected_state=%d actual_state=%d",
                             static_cast<int>(request.expected_state), static_cast<int>(actual_state));
                    continue;
                }
                const Status stop =
                    session_ ? session_->EndCapture() : Status::Error(ErrorCode::kUnavailable, "语音会话尚未启动");
                if (!stop.ok()) {
                    if (interaction_.state() != request.expected_state) {
                        ESP_LOGI(kTag, "BOARD_REQUEST_STALE kind=stop_capture expected_state=%d actual_state=%d",
                                 static_cast<int>(request.expected_state), static_cast<int>(interaction_.state()));
                        continue;
                    }
                    ESP_LOGW(kTag, "板级按键结束采集失败: %s", stop.message.c_str());
                    (void)EnqueueEvent(voice::VoiceInteractionEvent::kFailure);
                } else if (request.expected_state != voice::VoiceInteractionState::kFinalizing) {
                    // kFinalizing 必须等待最终 STT/TTS；其它停止来源按原状态安全回待机。
                    RestoreStandby(request);
                }
                continue;
            }
            if (request.kind == BoardRequestKind::kInterruptAndStartCapture) {
                if (!session_) {
                    ESP_LOGW(kTag, "板级打断后开始采集不可用");
                    RestoreStandbyFromWakeTask();
                    continue;
                }
                const Status interrupt = session_->Interrupt();
                const Status capture = interrupt.ok() ? session_->BeginCapture() : interrupt;
                if (!capture.ok()) {
                    ESP_LOGW(kTag, "板级打断后开始采集失败: %s", capture.message.c_str());
                    // 打断后启动失败：回待机，不显示"出错了/牛牛走了"。
                    (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
                }
                continue;
            }
            if (!session_ || !provider_) {
                ESP_LOGW(kTag, "本地唤醒确认请求不可用");
                RestoreStandbyFromWakeTask();
                continue;
            }
            // SparkBot 没有 AEC。普通唤醒先请求一次明确的“收到！”确认音，
            // 再按 detect -> listen.start 顺序进入同一 Linx 会话；VoiceSession
            // 会等确认 TTS 的 stop 或有界超时后才打开物理麦克风，避免自我介绍
            // 或确认音被采进首轮用户语音。
            const Status acknowledge = session_->NotifyLocalWakeWord(request.wake_word, "收到！");
            if (!acknowledge.ok()) {
                ESP_LOGW(kTag, "唤醒确认请求失败: %s", acknowledge.message.c_str());
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
                continue;
            }
            // Linx requires detect -> listen.start in one ordered control
            // sequence. Keep the physical input gated until the greeting TTS
            // ends; VoiceSession will reuse this Provider lease when the
            // interaction event loop later requests BeginCapture().
            const Status provider_capture = session_->BeginProviderCapture();
            if (!provider_capture.ok()) {
                ESP_LOGW(kTag, "唤醒后 Provider 监听启动失败: %s", provider_capture.message.c_str());
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kStandbyReady);
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
            case voice::VoiceInteractionState::kAcknowledging:
                return "收到";
            case voice::VoiceInteractionState::kOpeningCapture:
                return "准备中";
            case voice::VoiceInteractionState::kListening:
                return "聆听中";
            case voice::VoiceInteractionState::kFinalizing:
                return "处理中";
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
            case voice::VoiceInteractionState::kBooting:
                return voice::VoiceMood::kBooting;
            case voice::VoiceInteractionState::kStandby:
                return voice::VoiceMood::kIdle;
            case voice::VoiceInteractionState::kAcknowledging:
                return voice::VoiceMood::kNeutral;
            case voice::VoiceInteractionState::kOpeningCapture:
            case voice::VoiceInteractionState::kListening:
                return voice::VoiceMood::kListening;
            case voice::VoiceInteractionState::kFinalizing:
                return voice::VoiceMood::kThinking;
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

    static std::string CurrentStandbyStatusText() {
        const time_t now = time(nullptr);
        if (now <= 1600000000) return "空闲";  // 2020-09-13 之前视为尚未同步时钟。
        std::tm local{};
        localtime_r(&now, &local);
        char clock_text[8] = {};
        std::snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local.tm_hour, local.tm_min);
        return clock_text;
    }

    void CommitSnapshot() {
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
    void ShowDisplay(voice::VoiceMood mood, std::string_view status, std::string_view content) {
        EnqueueDisplayUpdate(mood, status, content, false);
    }

    // 临时 overlay 快照：由事件循环统一写入，revision 与业务快照保持严格单调。
    void ShowOverlay(voice::VoiceMood mood, std::string_view status, std::string_view content) {
        EnqueueDisplayUpdate(mood, status, content, true);
    }

    void StartOverlayTimer(uint32_t duration_ms) {
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
    void ClearExpiredWakeAck() {
        if (wake_ack_until_us_ == 0 || esp_timer_get_time() < wake_ack_until_us_) return;
        wake_ack_until_us_ = 0;
        if (snapshot_.phase != voice::VoiceInteractionState::kAcknowledging ||
            snapshot_.role != voice::VoiceContentRole::kSystem || snapshot_.content_text != "收到！") {
            return;
        }
        snapshot_.content_text.clear();
        snapshot_.role = voice::VoiceContentRole::kNone;
        ++snapshot_.revision;
        CommitSnapshot();
        ESP_LOGI(kTag, "WAKE_ACK_DISPLAY_EXPIRED=1");
    }

    Status HandleInteractionEvent(voice::VoiceInteractionEvent event, std::string_view wake_word = {}) {
        const auto transition = interaction_.Handle(event);
        if (!transition.ok() || !transition.value.has_value()) {
            ESP_LOGW(kTag, "忽略乱序板端交互事件=%d: %s", static_cast<int>(event), transition.status.message.c_str());
            return transition.status;
        }
#if CONFIG_VOICELIFE_SERIAL_VOICE_TEST
        ESP_LOGI(kTag, "SERIAL_VOICE_STATE event=%d state=%d action=%d", static_cast<int>(event),
                 static_cast<int>(interaction_.state()), static_cast<int>(transition.value->action));
#endif
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
        snapshot_.phase = interaction_.state();
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
        // WakeAck 租约：确认阶段显示“收到！”。麦克风只会在确认播报完整
        // 结束并经 kCaptureStarted 确认后打开。
        if ((event == voice::VoiceInteractionEvent::kWakeDetected ||
             event == voice::VoiceInteractionEvent::kInterruptAndAcknowledge) &&
            snapshot_.phase == voice::VoiceInteractionState::kAcknowledging && wake_ack_until_us_ > 0 &&
            esp_timer_get_time() < wake_ack_until_us_) {
            snapshot_.content_text = "收到！";
            snapshot_.role = voice::VoiceContentRole::kSystem;
        } else if (event == voice::VoiceInteractionEvent::kAcknowledgementTimedOut) {
            // 已放弃迟到的确认流，显示不能继续保留“收到！”。否则开麦后会
            // 出现状态栏为“准备中/聆听中”但内容仍是确认字幕的假反馈。
            wake_ack_until_us_ = 0;
            snapshot_.content_text.clear();
            snapshot_.role = voice::VoiceContentRole::kNone;
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
        switch (transition.value->action) {
            case voice::VoiceInteractionAction::kNone:
                return Status::Ok();
            case voice::VoiceInteractionAction::kStartCapture:
                if (QueueCaptureStart()) return Status::Ok();
                break;
            case voice::VoiceInteractionAction::kStartVoiceTurn:
                if (wake_word.empty()) {
                    return Status::Error(ErrorCode::kInvalidArgument, "本地唤醒词不能为空");
                }
                if (QueueVoiceTurn(wake_word)) return Status::Ok();
                break;
            case voice::VoiceInteractionAction::kStopVoiceTurn:
                if (QueueCaptureStop()) return Status::Ok();
                break;
            case voice::VoiceInteractionAction::kInterruptAndStartCapture:
                if (QueueInterruptAndCapture()) return Status::Ok();
                break;
            case voice::VoiceInteractionAction::kInterruptAndStartVoiceTurn:
                if (wake_word.empty()) {
                    return Status::Error(ErrorCode::kInvalidArgument, "本地打断词不能为空");
                }
                if (QueueInterruptAndVoiceTurn(wake_word)) return Status::Ok();
                break;
            case voice::VoiceInteractionAction::kRestoreStandby:
                // transport_disconnected 必须停在 kReconnecting；物理唤醒门可恢复，
                // 但不可用 kStandbyReady 把可见状态提前伪装为空闲。
                if (QueueStandbyRecovery(interaction_.state() != voice::VoiceInteractionState::kReconnecting)) {
                    return Status::Ok();
                }
                break;
            case voice::VoiceInteractionAction::kInterruptSession:
                if (QueueInterrupt()) return Status::Ok();
                break;
        }
        // 状态迁移已经发生但硬件动作没有被 WakeTask 接收。后续 kFailure
        // 将可见状态收口为 Error，而非永久显示 Listening/Interrupting。
        if (!EnqueueEvent(voice::VoiceInteractionEvent::kFailure)) {
            ESP_LOGE(kTag, "BOARD_REQUEST_FAILURE_EVENT_DROPPED=1");
        }
        return Status::Error(ErrorCode::kUnavailable, "板端语音控制请求未进入执行队列");
    }

    void LogVoiceEvidence(const voice::VoiceEvidence& evidence) { EnqueueVoiceEvidence(evidence); }

    void ProcessVoiceEvidence(const voice::VoiceEvidence& evidence) {
        // Evidence detail can contain STT text or service diagnostics. Emit
        // only lifecycle names and numeric counters needed for board review.
        if (evidence.event == "capture_started") {
            capture_started_us_.store(esp_timer_get_time());
            StartListenTimer(kListenStartTimeoutMs);
        } else if (evidence.event == "speech_started") {
            // The 6-second timer only bounds silence before the user begins.
            // Once VAD has observed real speech, it must not expire during a
            // long utterance or the later final-ASR stage.
            CancelListenTimer();
            ESP_LOGI(kTag, "VOICE_TIMEOUT_CANCELLED reason=speech_started");
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
        ESP_LOGI(kTag, "INTERACTION_QUEUE_STATS control_dropped=%u best_effort_dropped=%u board_dropped=%u",
                 static_cast<unsigned>(dropped_control_events_.load()),
                 static_cast<unsigned>(dropped_best_effort_events_.load()),
                 static_cast<unsigned>(dropped_board_requests_.load()));
        ESP_LOGI(kTag, "VOICE_EVENT session=%s generation=%llu event=%s detail_present=%d latency_from_capture_ms=%llu",
                 evidence.session_id.c_str(), static_cast<unsigned long long>(evidence.generation),
                 evidence.event.c_str(), evidence.detail.empty() ? 0 : 1, static_cast<unsigned long long>(latency_ms));
#if CONFIG_VOICELIFE_SERIAL_VOICE_TEST
        ESP_LOGI(kTag, "SERIAL_VOICE_EVIDENCE event=%s generation=%llu detail=%s", evidence.event.c_str(),
                 static_cast<unsigned long long>(evidence.generation), evidence.detail.c_str());
        if (evidence.event == "capture_started") {
            // 仅记录可注入窗口。排他模式只能由串口协议的 TURN_BEGIN 显式
            // 开启，普通实板唤醒不能因测试 Profile 而屏蔽物理麦克风。
            ESP_LOGI(kTag, "SERIAL_VOICE_CAPTURE_READY generation=%llu",
                     static_cast<unsigned long long>(evidence.generation));
        } else if (evidence.event == "capture_stopped") {
            if (assembly_ != nullptr && assembly_->test_audio_injection() != nullptr) {
                (void)assembly_->test_audio_injection()->SetTestInputEnabled(false);
            }
            ESP_LOGI(kTag, "SERIAL_VOICE_CAPTURE_CLOSED generation=%llu",
                     static_cast<unsigned long long>(evidence.generation));
        }
#endif
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
        } else if (evidence.event == "tts_first_audio" && wake_ack_requested_at_us_ > 0) {
            // Binary PCM can be delivered to the session before the event-loop
            // item for tts.start is processed. The first actual audio is still
            // the strongest proof that the greeting stream is alive, so it must
            // cancel the bounded wake-greeting timer in either ordering.
            CancelListenTimer();
            if (wake_ack_tts_started_at_us_ == 0) wake_ack_tts_started_at_us_ = esp_timer_get_time();
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
            const auto phase = interaction_.state();
            if (phase == voice::VoiceInteractionState::kListening ||
                phase == voice::VoiceInteractionState::kFinalizing ||
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
            // 确认 TTS 的 started 只说明服务端接收了请求，不能证明用户已经
            // 听到声音。首段 PCM 到达前保留 deadline，防止下行缓冲把首轮卡住。
            if (wake_ack_requested_at_us_ == 0) CancelListenTimer();
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kTtsStarted);
        } else if (evidence.event == "local_wake_detect_requested") {
            // detect 已进入 TX FIFO。Linx 可能随后发送本地唤醒问候 TTS；
            // 先确认协议顺序，再等待 tts.stop，超时才开启干净的用户采集。
            const auto mode = session_ != nullptr ? session_->config().mode : voice::VoiceMode::kManual;
            const char* mode_name = "manual";
            if (mode == voice::VoiceMode::kAuto) {
                mode_name = "auto";
            } else if (mode == voice::VoiceMode::kRealtime) {
                mode_name = "realtime";
            }
            ESP_LOGI(kTag, "LOCAL_WAKE_PROTOCOL_ACCEPTED action=await_greeting mode=%s", mode_name);
            (void)EnqueueEvent(voice::VoiceInteractionEvent::kWakeDetectionAccepted);
            StartListenTimer(kWakeAckFirstAudioTimeoutMs);
        } else if (evidence.event == "local_wake_ack_requested" || evidence.event == "interrupt_ack_requested") {
            // 本地唤醒/打断确认已提交给 Provider。直到首段 PCM 到达前保留
            // deadline；超时后直接开始采集，不能无限等待远端音频。
            if (evidence.event == "interrupt_ack_requested") {
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kInterruptAcknowledged);
            }
            StartListenTimer(kWakeAckFirstAudioTimeoutMs);
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
            // 服务端可能先送文本字幕，数秒后才送 PCM。确认阶段只有实际
            // tts_first_audio 才能解除 deadline，不能把字幕当成已播放。
            if (wake_ack_requested_at_us_ == 0) CancelListenTimer();
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
            if (interaction_.state() != voice::VoiceInteractionState::kSpeaking) {
                ESP_LOGI(kTag, "TTS_STOPPED_STALE state=%d 丢弃迟到结束事件", static_cast<int>(interaction_.state()));
                return;
            }
            if (terminal_turn_ || binding_turn_awaiting_tts_completion_) {
                // 告别或绑定码播报完成后直接恢复待机。绑定码页面会在
                // HandleInteractionEvent 的待机呈现规则中立即恢复。
                if (assembly_ != nullptr && assembly_->uses_local_wake_detector()) {
                    assembly_->wake_gate().SuppressLocalWakeFor(kTerminalWakeGuardMs);
                    ESP_LOGI(kTag, "WAKE_GUARD_ARMED ms=%u reason=terminal_tts",
                             static_cast<unsigned>(kTerminalWakeGuardMs));
                }
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
            const auto phase = interaction_.state();
            if (phase != voice::VoiceInteractionState::kStandby) {
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kFailure);
            }
        } else if (evidence.event == "capture_stopped") {
            // EndCapture may be caused by button release instead of local
            // VAD. Arm a complete final-ASR window only after the physical
            // input and provider have both accepted listen.stop; retaining
            // the original pre-speech timer cuts long utterances short.
            if (interaction_.state() == voice::VoiceInteractionState::kFinalizing) {
                StartListenTimer(kFinalSttTimeoutMs);
                ESP_LOGI(kTag, "FINAL_STT_TIMEOUT_REARMED ms=%u source=capture_stopped",
                         static_cast<unsigned>(kFinalSttTimeoutMs));
            } else {
                CancelListenTimer();
            }
        } else if (evidence.event == "vad_silence") {
            // 本地 VAD 端点：用户说完话后达到 VoiceSession 配置的静音窗口，发 listen.stop 使服务端
            // 进入最终 STT，然后等待最终 STT（kFinalizing），不回待机。
            // 启动 5s 最终 STT 超时：无 STT 则 abort 收尾。
            CancelListenTimer();
            if (interaction_.state() == voice::VoiceInteractionState::kListening) {
                (void)EnqueueEvent(voice::VoiceInteractionEvent::kEndpointDetected);
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
    EspPairingClock im_pairing_clock_;
    im::BindingUseCase binding_use_case_;
    BindingPollingLease binding_poll_lease_;
    bool binding_display_active_ = false;
    uint64_t binding_display_generation_ = 0;
    std::string binding_status_text_;
    std::string binding_content_text_;
    std::optional<BindingPresentation> deferred_binding_presentation_;
    std::string deferred_binding_speech_;
    std::atomic_bool im_lifecycle_reserved_{false};
    std::atomic_bool im_lifecycle_activated_{false};
    bool im_system_time_logged_ = false;
    TaskHandle_t im_lifecycle_task_ = nullptr;
    mcp::McpServer mcp_server_;
    schedule::ScheduleOperationService schedule_operation_service_;
    schedule::ScheduleService schedule_service_;
    schedule::ScheduleRuleService schedule_rule_service_;
    bool schedule_mcp_registered_ = false;
    std::unique_ptr<timing_esp::EspTimingTaskRuntime> timing_runtime_;
    std::unique_ptr<ReminderSpeech> reminder_speech_;
    std::unique_ptr<ImScheduleReminderNotification> reminder_notification_;
    std::unique_ptr<schedule::ScheduleReminderService> schedule_reminder_service_;
    std::unique_ptr<ImScheduleReminderActionExecutor> reminder_action_executor_;
    EspScheduleReminderClock reminder_action_clock_;
    std::mutex im_action_mutex_;
    std::deque<im::ActionWindow> im_action_windows_;
    std::atomic_int im_action_worker_count_{0};
    std::atomic_bool im_action_stop_{false};
    std::atomic_bool im_action_worker_stopped_{false};
    TaskHandle_t im_action_worker_task_ = nullptr;
    std::string im_gateway_origin_;
    Status init_status_ = Status::Ok();
    linx::LinxJsonCodec linx_codec_;
    linx::LinxConnectionConfig linx_config_;
    std::unique_ptr<linx_esp::EspWebSocketTransport> linx_transport_ =
        std::make_unique<linx_esp::EspWebSocketTransport>(linx_secrets_);
#if CONFIG_VOICELIFE_SERIAL_VOICE_TEST
    std::unique_ptr<SerialVoiceTest> serial_voice_test_;
#endif
    QueueHandle_t wake_queue_ = nullptr;
    TaskHandle_t wake_task_ = nullptr;
#if CONFIG_VOICELIFE_STATE_FLOW_TEST
    TaskHandle_t state_flow_task_ = nullptr;
#endif
    // 交互事件单写者（InteractionEventLoop）：外部线程只投递事件。
    struct InteractionEventItem {
        voice::VoiceInteractionEvent event = voice::VoiceInteractionEvent::kBootCompleted;
        std::string wake_word;
        /** @brief 纯显示刷新文本（display_only 时由事件循环应用，不走状态机）。 */
        std::string display_text;
        /** @brief 是否为纯显示刷新（跳过 HandleInteractionEvent）。 */
        bool display_only = false;
        /** 受控系统显示更新；只由事件循环转换为 DisplaySnapshot。 */
        bool display_update = false;
        /** 是否为临时 overlay（音量/告别）。 */
        bool display_overlay = false;
        voice::VoiceMood display_mood = voice::VoiceMood::kIdle;
        std::string display_status;
        std::string display_content;
        /** VoiceSession/Provider 回调携带的业务事实，由事件循环处理。 */
        bool voice_evidence = false;
        voice::VoiceEvidence evidence;
        /** MCP/轮询任务产生的脱敏绑定结果；事件循环负责呈现与播报。 */
        bool binding_result = false;
        im::BindingResult binding;
        /** Runtime 依赖重绑后清除旧 pending 呈现。 */
        bool binding_reset = false;
        uint64_t binding_generation = 0;
        /** esp_timer 只投递，事件循环根据当前状态决定超时收尾。 */
        bool listen_timeout = false;
        /** 启动/网络回调携带的受控连接事实。 */
        bool network_update = false;
        bool network_connected = false;
        /** 板级输入适配器的纯语义事件；由事件循环转换为状态或音量变更。 */
        bool board_input = false;
        BoardInputAction board_action = BoardInputAction::kToggleChat;
    };
    enum class EmergencyEvidenceKind : uint8_t {
        kCaptureStarted = 0,
        kInterruptAcknowledged = 1,
    };
    static constexpr std::size_t kInteractionEventCount =
        static_cast<std::size_t>(voice::VoiceInteractionEvent::kFailure) + 1U;
    struct EmergencyStateEvent {
        bool pending = false;
        uint64_t sequence = 0;
        std::string wake_word;
    };
    struct EmergencyEvidenceEvent {
        bool pending = false;
        uint64_t sequence = 0;
        voice::VoiceEvidence evidence;
    };
    // State-driving events must never be evicted by display text or telemetry.
    // Both queues are bounded: the event loop owns all state mutation, while
    // producers remain non-blocking on real-time callback paths.
    static constexpr std::size_t kControlEventQueueCapacity = 16;
    static constexpr std::size_t kBestEffortEventQueueCapacity = 4;
    std::deque<InteractionEventItem> control_event_queue_;
    std::deque<InteractionEventItem> best_effort_event_queue_;
    // 满载时不分配、不等待的状态事实通道。按类型合并重复事件，保留最新
    // wake word/evidence；正常队列恢复后由事件循环按投递顺序优先消费。
    std::array<EmergencyStateEvent, kInteractionEventCount> emergency_state_events_{};
    std::array<EmergencyEvidenceEvent, 2> emergency_evidence_events_{};
    uint64_t emergency_event_sequence_ = 0;
    std::atomic<uint32_t> dropped_control_events_{0};
    std::atomic<uint32_t> dropped_best_effort_events_{0};
    std::atomic<uint32_t> dropped_board_requests_{0};
    mutable std::mutex event_mutex_;
    std::condition_variable event_cv_;
    TaskHandle_t event_task_ = nullptr;
    bool event_loop_stop_ = false;
    bool event_loop_stopped_ = false;
    /** @brief 音量 overlay 到期标志（timer 置位，事件循环消费）。 */
    std::atomic<bool> overlay_expired_{false};
    std::mutex mcp_mutex_;
    std::condition_variable mcp_cv_;
    std::deque<std::shared_ptr<McpRequest>> mcp_queue_;
    TaskHandle_t mcp_task_ = nullptr;
    bool mcp_stop_ = false;
    std::atomic_bool mcp_stopped_{true};

    [[nodiscard]] static bool IsBestEffortEvent(const InteractionEventItem& item) {
        if (item.display_only || item.network_update || item.display_update) return true;
        if (!item.voice_evidence) return false;
        // These evidence events only update observability or the current
        // display. Lifecycle and state-machine evidence remains control work.
        return item.evidence.event == "tts_first_audio" || item.evidence.event == "tts_sentence_started" ||
               item.evidence.event == "mcp_tool_result" || item.evidence.event == "mcp_tool_failed" ||
               item.evidence.event == "ready" || item.evidence.event == "stale_event_dropped" ||
               item.evidence.event == "interrupted" || item.evidence.event == "interrupt_fence_reached" ||
               item.evidence.event == "stopped";
    }

    [[nodiscard]] static std::optional<EmergencyEvidenceKind> EmergencyEvidenceKindFor(
        const voice::VoiceEvidence& evidence) {
        if (evidence.event == "capture_started") return EmergencyEvidenceKind::kCaptureStarted;
        if (evidence.event == "interrupt_ack_requested") return EmergencyEvidenceKind::kInterruptAcknowledged;
        return std::nullopt;
    }

    [[nodiscard]] bool HasEmergencyEventLocked() const {
        return std::any_of(emergency_state_events_.begin(), emergency_state_events_.end(),
                           [](const EmergencyStateEvent& item) { return item.pending; }) ||
               std::any_of(emergency_evidence_events_.begin(), emergency_evidence_events_.end(),
                           [](const EmergencyEvidenceEvent& item) { return item.pending; });
    }

    bool EnqueueEmergencyEvent(voice::VoiceInteractionEvent event, std::string wake_word) {
        const std::size_t index = static_cast<std::size_t>(event);
        if (index >= emergency_state_events_.size()) return false;
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            auto& item = emergency_state_events_[index];
            item.pending = true;
            item.sequence = ++emergency_event_sequence_;
            item.wake_word = std::move(wake_word);
        }
        event_cv_.notify_one();
        return true;
    }

    bool EnqueueEmergencyEvidence(InteractionEventItem item) {
        const auto kind = EmergencyEvidenceKindFor(item.evidence);
        if (!kind.has_value()) return false;
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            auto& pending = emergency_evidence_events_[static_cast<std::size_t>(*kind)];
            pending.pending = true;
            pending.sequence = ++emergency_event_sequence_;
            pending.evidence = std::move(item.evidence);
        }
        event_cv_.notify_one();
        return true;
    }

    bool PopEmergencyEventLocked(InteractionEventItem* destination) {
        std::optional<std::size_t> state_index;
        std::optional<std::size_t> evidence_index;
        uint64_t earliest_sequence = UINT64_MAX;
        for (std::size_t index = 0; index < emergency_state_events_.size(); ++index) {
            const auto& pending = emergency_state_events_[index];
            if (pending.pending && pending.sequence < earliest_sequence) {
                state_index = index;
                evidence_index.reset();
                earliest_sequence = pending.sequence;
            }
        }
        for (std::size_t index = 0; index < emergency_evidence_events_.size(); ++index) {
            const auto& pending = emergency_evidence_events_[index];
            if (pending.pending && pending.sequence < earliest_sequence) {
                state_index.reset();
                evidence_index = index;
                earliest_sequence = pending.sequence;
            }
        }
        if (state_index.has_value()) {
            auto& pending = emergency_state_events_[*state_index];
            destination->event = static_cast<voice::VoiceInteractionEvent>(*state_index);
            destination->wake_word = std::move(pending.wake_word);
            pending.pending = false;
            pending.sequence = 0;
            return true;
        }
        if (evidence_index.has_value()) {
            auto& pending = emergency_evidence_events_[*evidence_index];
            destination->voice_evidence = true;
            destination->evidence = std::move(pending.evidence);
            pending.pending = false;
            pending.sequence = 0;
            return true;
        }
        return false;
    }

    /** @brief 非阻塞投递：控制事件独立于显示/诊断背压，并优先被消费。 */
    bool EnqueueInteractionItem(InteractionEventItem& item) {
        const bool best_effort = IsBestEffortEvent(item);
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            auto& queue = best_effort ? best_effort_event_queue_ : control_event_queue_;
            const std::size_t capacity = best_effort ? kBestEffortEventQueueCapacity : kControlEventQueueCapacity;
            if (queue.size() >= capacity) {
                if (best_effort) {
                    queue.pop_front();
                    ++dropped_best_effort_events_;
                } else {
                    const uint32_t dropped = ++dropped_control_events_;
                    ESP_LOGW(kTag, "INTERACTION_CONTROL_QUEUE_FULL dropped=%u", static_cast<unsigned>(dropped));
                    return false;
                }
            }
            queue.push_back(std::move(item));
        }
        event_cv_.notify_one();
        return true;
    }

    /** @brief 投递交互事件；任何线程可调用。 */
    bool EnqueueEvent(voice::VoiceInteractionEvent event, std::string_view wake_word = {}) {
        InteractionEventItem item{};
        item.event = event;
        item.wake_word = std::string(wake_word);
        if (EnqueueInteractionItem(item)) return true;
        return EnqueueEmergencyEvent(event, std::move(item.wake_word));
    }

    /** @brief 投递纯显示刷新（TTS 文本等，事件循环内应用，不触发状态机）。 */
    void EnqueueDisplayText(std::string detail) {
        InteractionEventItem item{};
        item.display_only = true;
        item.display_text = std::move(detail);
        EnqueueInteractionItem(item);
    }

    /** @brief 投递启动/错误/overlay 等系统语义；不携带硬件资源或原始数据。 */
    void EnqueueDisplayUpdate(voice::VoiceMood mood, std::string_view status, std::string_view content, bool overlay) {
        InteractionEventItem item{};
        item.display_update = true;
        item.display_overlay = overlay;
        item.display_mood = mood;
        item.display_status = std::string(status);
        item.display_content = std::string(content);
        EnqueueInteractionItem(item);
    }

    void EnqueueBindingResult(const im::BindingResult& result) {
        InteractionEventItem item{};
        item.binding_result = true;
        item.binding = result;
        EnqueueInteractionItem(item);
    }

    void EnqueueBindingReset(uint64_t generation) {
        InteractionEventItem item{};
        item.binding_reset = true;
        item.binding_generation = generation;
        EnqueueInteractionItem(item);
    }

    void CancelBindingTerminalDisplay() {
        binding_terminal_display_active_ = false;
        binding_terminal_resume_listening_ = false;
        binding_terminal_until_us_ = 0;
        binding_terminal_status_text_.clear();
        binding_terminal_content_text_.clear();
    }

    void ClearExpiredBindingTerminalDisplay() {
        if (!binding_terminal_display_active_ || binding_terminal_until_us_ == 0 ||
            esp_timer_get_time() < binding_terminal_until_us_) {
            return;
        }
        const bool resume_listening = binding_terminal_resume_listening_;
        CancelBindingTerminalDisplay();
        deferred_binding_speech_.clear();
        if (interaction_.state() != voice::VoiceInteractionState::kStandby) return;
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

    void CommitBindingPresentation(const BindingPresentation& presentation) {
        snapshot_.mood =
            presentation.content_text == "绑定成功" ? voice::VoiceMood::kHappy : voice::VoiceMood::kNeutral;
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

    void QueueDeferredBindingSpeechIfStandby() {
        if (interaction_.state() != voice::VoiceInteractionState::kStandby) return;
        if (deferred_binding_presentation_.has_value()) {
            CommitBindingPresentation(*deferred_binding_presentation_);
            deferred_binding_presentation_.reset();
        }
        if (deferred_binding_speech_.empty()) return;
        std::string speech = std::move(deferred_binding_speech_);
        deferred_binding_speech_.clear();
        if (!QueueSystemSpeech(speech)) deferred_binding_speech_ = std::move(speech);
    }

    void ProcessBindingResult(const im::BindingResult& result) {
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

        if (ShouldEndVoiceTurnAfterBindingResult(result,
                                                 interaction_.state() != voice::VoiceInteractionState::kStandby)) {
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
        if (!presentation.keep_visible && interaction_.state() != voice::VoiceInteractionState::kStandby) {
            deferred_binding_presentation_ = presentation;
            deferred_binding_speech_ = presentation.speech_text;
            return;
        }

        CommitBindingPresentation(presentation);
        if (!presentation.announce) return;
        if (interaction_.state() == voice::VoiceInteractionState::kStandby) {
            if (!QueueSystemSpeech(presentation.speech_text)) deferred_binding_speech_ = presentation.speech_text;
        } else {
            // 活跃 MCP 回合的响应已携带 speak_text，由 Provider 播报一次。
            // 不再延迟本地重复播报；该播报结束后会直接回待机显示绑定码。
        }
    }

    void EnqueueVoiceEvidence(const voice::VoiceEvidence& evidence) {
        InteractionEventItem item{};
        item.voice_evidence = true;
        item.evidence = evidence;
        if (EnqueueInteractionItem(item)) return;
        if (!EnqueueEmergencyEvidence(std::move(item))) {
            ESP_LOGW(kTag, "VOICE_EVIDENCE_DROPPED event=%s", evidence.event.c_str());
        }
    }

    void EnqueueListenTimeout() {
        InteractionEventItem item{};
        item.listen_timeout = true;
        EnqueueInteractionItem(item);
    }

    void EnqueueNetworkState(bool connected) {
        InteractionEventItem item{};
        item.network_update = true;
        item.network_connected = connected;
        EnqueueInteractionItem(item);
    }

    /** @brief 事件循环任务入口（唯一调用 HandleInteractionEvent 的线程）。 */
    static void EventLoopTaskEntry(void* arg) { static_cast<Runtime*>(arg)->EventLoopLoop(); }

    /** @brief 事件循环：消费事件 -> 状态迁移 -> 快照 -> 显示提交。 */
    void EventLoopLoop() {
#ifdef ESP_PLATFORM
        while (true) {
            InteractionEventItem item;
            {
                std::unique_lock<std::mutex> lock(event_mutex_);
                event_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return event_loop_stop_ || HasEmergencyEventLocked() || !control_event_queue_.empty() ||
                           !best_effort_event_queue_.empty();
                });
                if (event_loop_stop_ && !HasEmergencyEventLocked() && control_event_queue_.empty() &&
                    best_effort_event_queue_.empty()) {
                    break;
                }
                const bool has_emergency_event = PopEmergencyEventLocked(&item);
                if (!has_emergency_event && control_event_queue_.empty() && best_effort_event_queue_.empty()) {
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
                if (!has_emergency_event) {
                    auto& queue = !control_event_queue_.empty() ? control_event_queue_ : best_effort_event_queue_;
                    item = std::move(queue.front());
                    queue.pop_front();
                }
            }
            // provider_error 等事件持续占满队列时，终态租约仍必须按时收口。
            ClearExpiredBindingTerminalDisplay();
            if (item.display_only) {
                // 纯显示刷新：仅当控制器处于 kSpeaking 时应用（迟到的 TTS 丢弃）。
                if (interaction_.state() == voice::VoiceInteractionState::kSpeaking && !item.display_text.empty()) {
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
                    if (interaction_.state() == voice::VoiceInteractionState::kStandby) {
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
                if (interaction_.state() == voice::VoiceInteractionState::kAcknowledging) {
                    // The Provider listen.start was already sent immediately
                    // after detect. Timeout only opens the physical input; an
                    // abort/restart here would create a second protocol turn.
                    ESP_LOGW(kTag, "ACK_FIRST_AUDIO_TIMEOUT transition=acknowledging->opening_capture");
                    wake_ack_requested_at_us_ = 0;
                    wake_ack_tts_started_at_us_ = 0;
                    wake_ack_until_us_ = 0;
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kAcknowledgementTimedOut);
                } else if (interaction_.state() == voice::VoiceInteractionState::kSpeaking &&
                           wake_ack_requested_at_us_ > 0) {
                    ESP_LOGW(kTag, "ACK_FIRST_AUDIO_TIMEOUT transition=speaking->opening_capture");
                    if (session_) (void)session_->Interrupt();
                    // Interrupt 使旧确认流失效，迟到的 tts.stop 会由会话层
                    // 丢弃；这里必须立即清除归因，避免下一轮正常回复被误算
                    // 为唤醒确认，或把再次开始的监听计时器取消掉。
                    wake_ack_requested_at_us_ = 0;
                    wake_ack_tts_started_at_us_ = 0;
                    wake_ack_until_us_ = 0;
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kAcknowledgementTimedOut);
                } else if (interaction_.state() == voice::VoiceInteractionState::kListening) {
                    // 实机麦克风底噪可能让本地 VAD 未能识别静音端点，但此前
                    // 已采集的语音仍必须以 listen.stop 交给服务端完成最终 STT。
                    // 直接 abort 会无条件丢弃该回合，表现为“收到后不再回应”。
                    ESP_LOGI(kTag, "LISTEN_TIMEOUT transition=listening->finalizing");
                    (void)HandleInteractionEvent(voice::VoiceInteractionEvent::kEndpointDetected);
                    StartListenTimer(kFinalSttTimeoutMs);
                } else if (interaction_.state() == voice::VoiceInteractionState::kFinalizing) {
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
                // 唤醒前置（唯一状态写者内）：普通唤醒和“别说了”都记录
                // 确认播报时延；是否开麦由对应的 TTS 完成事件决定。
                const int64_t now = esp_timer_get_time();
                wake_ack_requested_at_us_ = now;
                wake_ack_tts_started_at_us_ = 0;
                wake_ack_until_us_ = now + kWakeAckDisplayUs;
            }
            if (item.event == voice::VoiceInteractionEvent::kWakeDetectionAccepted) {
                // Keep the wake-greeting timing lease alive. A detect has been
                // accepted by TX, but the microphone remains closed until the
                // optional server greeting finishes or the bounded timer fires.
                ESP_LOGI(kTag, "LOCAL_WAKE_GREETING_WAIT state=%d timeout_ms=%u",
                         static_cast<int>(interaction_.state()), static_cast<unsigned>(kWakeAckFirstAudioTimeoutMs));
            }
            const Status wake_status = HandleInteractionEvent(item.event, item.wake_word);
            if (item.event != voice::VoiceInteractionEvent::kWakeDetected && !wake_status.ok()) {
                ESP_LOGW(kTag, "INTERACTION_REJECTED event=%d state=%d err=%s", static_cast<int>(item.event),
                         static_cast<int>(interaction_.state()), wake_status.message.c_str());
            }
            if ((item.event == voice::VoiceInteractionEvent::kWakeDetected ||
                 item.event == voice::VoiceInteractionEvent::kInterruptAndAcknowledge ||
                 item.event == voice::VoiceInteractionEvent::kInterruptRequested) &&
                !wake_status.ok()) {
                // 非法本地命令（例如待机时“别说了”）不得让检测器停死。
                ESP_LOGW(kTag, "LOCAL_COMMAND_REJECTED state=%d err=%s", static_cast<int>(interaction_.state()),
                         wake_status.message.c_str());
                if (assembly_->uses_local_wake_detector()) {
                    (void)assembly_->wake_gate().StartStandby();
                }
            }
        }
        event_loop_stopped_ = true;
        vTaskDelete(nullptr);
#endif
    }
    // Keep production at 70. Serial voice validation runs at half volume so
    // physical-board stress tests do not disturb the surrounding environment.
#if defined(ESP_PLATFORM) && CONFIG_VOICELIFE_SERIAL_VOICE_TEST
    int volume_ = 35;
#else
    int volume_ = 70;
#endif
    std::atomic<int64_t> capture_started_us_{0};
    std::string stt_display_text_;
    // 下行内容滚动窗口起始字符（0=从头）；滚动迁移至 Ssd1306PresentationAdapter。
    // 本轮是否为终止回合（用户说“再见/拜拜”等）：播报结束后不进入 follow-up。
    bool terminal_turn_ = false;
    bool binding_turn_awaiting_tts_completion_ = false;
    // 绑定成功/失败等终态页面的独立显示租约；只由事件循环读写。
    bool binding_terminal_display_active_ = false;
    bool binding_terminal_resume_listening_ = false;
    voice::VoiceMood binding_terminal_mood_ = voice::VoiceMood::kNeutral;
    std::string binding_terminal_status_text_;
    std::string binding_terminal_content_text_;
    int64_t binding_terminal_until_us_ = 0;
    int64_t wake_ack_requested_at_us_ = 0;
    int64_t wake_ack_tts_started_at_us_ = 0;
    // WakeAck 显示租约截止时刻（esp_timer_us）：到期前下行栏显示“收到！”。
    int64_t wake_ack_until_us_ = 0;
    // 音量 overlay 截止时刻（esp_timer_us）：到期后恢复最新快照。
    int64_t volume_overlay_until_us_ = 0;
    esp_timer_handle_t volume_overlay_timer_ = nullptr;
    // 显示模型快照：会话阶段 → 可见状态的推导结果；revision 驱动增量重绘。
    voice::DisplaySnapshot snapshot_;
    /** 临时 overlay 覆盖前的业务快照；到期后由事件循环恢复。 */
    voice::DisplaySnapshot overlay_base_snapshot_;
    bool overlay_active_ = false;
    uint64_t last_rendered_revision_ = 0;
    // 构建期选定的平台装配（显示语义提交目标）。
    PlatformAssembly* assembly_ = nullptr;
    esp_timer_handle_t listen_timer_ = nullptr;
#else
    ScaffoldAudioInput audio_input_;
    ScaffoldAudioOutput audio_output_;
#endif
    // 仅完成依赖装配，现有事件循环尚未迁移到该路径。
    application::InteractionOrchestrator interaction_orchestrator_;
    runtime_esp::EspInteractionTaskHost interaction_task_host_{interaction_orchestrator_};
    voice::VoiceInteractionController interaction_;
    std::unique_ptr<voice::SpeechProviderAdapter> provider_;
    std::unique_ptr<voice::VoiceSession> session_;

   public:
    ~Runtime() {
#ifdef ESP_PLATFORM
        StopScheduleReminderRuntime();
#endif
    }

    Status RequestInterrupt() {
        if (!session_) return Status::Error(ErrorCode::kUnavailable, "设备运行时尚未启动");
#ifdef ESP_PLATFORM
        EnqueueEvent(voice::VoiceInteractionEvent::kInterruptRequested);
        return Status::Ok();  // 事件已投递，状态迁移由事件循环执行。
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

Status Start(PlatformAssembly& assembly) { return Instance().Start(assembly); }

Status RequestInterrupt() { return Instance().RequestInterrupt(); }

}  // namespace voicelife::runtime
