#include "esp_runtime_internal.h"
#include "voicelife/runtime/runtime.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "im_binding_mcp_tools.h"
#include "linx_mcp_bridge.h"
#include "linx_ota_bootstrap.h"
#include "mcp_worker_policy.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "voicelife/mcp/schedule_mcp_tools.h"

namespace voicelife::runtime {
namespace {
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
}  // namespace

Result<std::string> NvsSecretResolver::Resolve(std::string_view reference) {
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
    const esp_err_t open_error =
        nvs_open_from_partition(LinxSecretPartitionLabel(), path.substr(0, separator).c_str(), NVS_READONLY, &handle);
    if (open_error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, "Linx token NVS 命名空间不可用");
    }
    auto result = ReadNvsString(handle, path.substr(separator + 1).c_str());
    nvs_close(handle);
    return result;
#endif
}

#endif

Result<voice::VoiceAudioFormats> ScaffoldSpeechProvider::audio_formats() const {
    voice::VoiceAudioFormats fmt;
    fmt.capture = voice::AudioFormat{};
    fmt.playback = voice::AudioFormat{};
    return Result<voice::VoiceAudioFormats>::Success(fmt);
}
/** @brief 构造运行时并将日程服务绑定到持久化仓储。 */
Runtime::Runtime()
#ifdef ESP_PLATFORM
    : schedule_service_(storage_.GetScheduleRepository()),
      schedule_operation_service_(storage_.GetScheduleOperationRepository()),
      schedule_rule_service_(storage_.GetScheduleRuleRepository(), storage_.GetScheduleExceptionRepository(),
                             storage_.GetScheduleRepository())
#endif
{
    auto& registry = voice::SpeechProviderRegistry::Instance();
#ifdef ESP_PLATFORM
    init_status_ = mcp::RegisterScheduleMcpTools(mcp_server_, schedule_service_, schedule_rule_service_);
    if (init_status_.ok()) {
        // MCP worker 只产生绑定结果；轮询与 OLED/TTS 均由各自受控任务处理。
        init_status_ =
            RegisterImBindingMcpTools(mcp_server_, binding_use_case_, [this](const im::BindingResult& result) {
                EnqueueBindingResult(result);
                if (result.state == im::BindingState::kPending) StartBindingPolling(result.generation);
            });
    }
    if (init_status_.ok()) {
        ESP_LOGI(kTag,
                 "MCP_TOOLS_READY count=5 names=schedule.create,schedule.query,schedule.update,schedule.delete,"
                 "im.binding.start");
    }
    registry.Register("xrobot-websocket", linx::LinxSpeechProviderAdapter::DefaultCapabilities(), [this]() {
        return std::make_unique<linx::LinxSpeechProviderAdapter>(
            *linx_transport_, linx_codec_, linx_config_, linx::LinxSpeechProviderAdapter::DefaultCapabilities(),
            [this](std::string_view payload, std::string_view session_id) {
                return HandleMcpRequest(payload, session_id);
            });
    });
#endif
    registry.Register("scaffold", voice::CapabilityProfile{"scaffold", {"streaming-asr", "tts"}},
                      []() { return std::make_unique<ScaffoldSpeechProvider>(); });
}

Status Runtime::Start(PlatformAssembly& assembly) {
    assembly_ = &assembly;
    const auto fail_startup = [this](Status status) {
#ifdef ESP_PLATFORM
        StopMcpWorker();
        StopEventLoop();
#endif
        return status;
    };
    auto& registry = voice::SpeechProviderRegistry::Instance();
    if (!init_status_.ok()) return init_status_;
    const Status storage_status = storage_.Start();
    if (!storage_status.ok()) return storage_status;
#ifdef ESP_PLATFORM
    // 立创实战派 ESP32-S3 板载 WS2812 灯珠接 GPIO48（小智 BUILTIN_LED_GPIO）。
    // 主 NVS 分区初始化（Wi-Fi 驱动/凭据等依赖；linx_secrets 为加密分区另行初始化）。
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
    }
    // 板级 LED 初始化（板型专属，Assembly 持有）。
    assembly_->InitializeBoardLeds();
    if (const Status display_status = assembly_->Start(); !display_status.ok()) {
        ESP_LOGE(kTag, "STARTUP_ERROR stage=display_start code=%d msg=%s", static_cast<int>(display_status.code),
                 display_status.message.c_str());
        return display_status;
    }
    // 显示启动后立即启动唯一的交互/显示语义写者。此后的启动、网络、音量
    // 和会话事件均只投递到该循环，不允许 Runtime 直接 Render。
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_queue_.clear();
        event_loop_stop_ = false;
        event_loop_stopped_ = false;
    }
    if (xTaskCreate(&Runtime::EventLoopTaskEntry, "voicelife_interaction", 8192, this, 5, &event_task_) != pdPASS) {
        return Status::Error(ErrorCode::kInternal, "创建交互事件循环任务失败");
    }
    if (const Status mcp_worker = StartMcpWorker(); !mcp_worker.ok()) {
        return fail_startup(mcp_worker);
    }
    ShowDisplay(voice::VoiceMood::kConnecting, "联网", "");
    if (const Status secret_store = InitializeLinxSecretStore(); !secret_store.ok()) {
        ESP_LOGW(kTag, "STARTUP_ERROR stage=secret_store code=%d", static_cast<int>(secret_store.code));
        ShowDisplay(voice::VoiceMood::kSad, "错误", "");
        return fail_startup(secret_store);
    }
#if CONFIG_VOICELIFE_IM_GATEWAY
    // USB IM provisioning 不依赖 Wi-Fi；即使网络配置缺失并进入 SoftAP，也必须开放物理恢复窗口。
    if (!StartImProvisioningTask()) {
        ESP_LOGW(kTag, "IM_PROVISION_TASK_FAILED=1");
    }
#endif
    auto connection =
        BootstrapLinxOtaConfig(assembly_->board_identity(), [this](std::string_view title, std::string_view detail) {
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
    // IM 的 SNTP、Gateway 探针和退避全部在独立任务中完成，语音启动路径不等待网络。
    StartImRuntime();
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
    assembly_->SetOutputVolume(static_cast<uint8_t>(volume_));
    if (assembly_->uses_local_wake_detector()) {
        assembly_->wake_gate().SetWakeSink([this](std::string_view wake_word) { QueueWakeWord(wake_word); });
    }
    session_ = std::make_unique<voice::VoiceSession>(
        assembly_->wake_gate(), assembly_->audio_output(), *provider_,
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
        ShowDisplay(voice::VoiceMood::kSad, "错误", "");
        return fail_startup(session_status);
    }

#ifdef ESP_PLATFORM
    if (wake_queue_ == nullptr) {
        wake_queue_ = xQueueCreate(4, sizeof(BoardRequest));
        if (wake_queue_ == nullptr) return fail_startup(Status::Error(ErrorCode::kInternal, "创建唤醒队列失败"));
        const BaseType_t task_status =
            xTaskCreate(&Runtime::WakeTaskEntry, "voicelife_wake", 4096, this, 5, &wake_task_);
        if (task_status != pdPASS) return fail_startup(Status::Error(ErrorCode::kInternal, "创建唤醒控制任务失败"));
    }
    EnqueueEvent(voice::VoiceInteractionEvent::kBootCompleted);
    const Status input_status =
        assembly_->StartBoardInput([this](BoardInputAction action) { EnqueueBoardInput(action); });
    if (!input_status.ok()) return fail_startup(input_status);
#if CONFIG_VOICELIFE_STATE_FLOW_TEST
    if (const Status state_flow_status = StartStateFlowDiagnostic(); !state_flow_status.ok()) {
        return fail_startup(state_flow_status);
    }
#endif
#endif
    return Status::Ok();
}

Status Runtime::RequestInterrupt() {
    if (!session_) return Status::Error(ErrorCode::kUnavailable, "设备运行时尚未启动");
#ifdef ESP_PLATFORM
    EnqueueEvent(voice::VoiceInteractionEvent::kInterruptRequested);
    return Status::Ok();
#else
    return Status::Error(ErrorCode::kUnavailable, "板端打断仅支持 ESP 平台");
#endif
}

Runtime& Instance() {
    static Runtime runtime;
    return runtime;
}

Status Start(PlatformAssembly& assembly) { return Instance().Start(assembly); }
Status RequestInterrupt() { return Instance().RequestInterrupt(); }
}  // namespace voicelife::runtime

namespace voicelife::runtime_esp {
Status Start(runtime::PlatformAssembly& assembly) { return runtime::Start(assembly); }
Status RequestInterrupt() { return runtime::RequestInterrupt(); }
}  // namespace voicelife::runtime_esp
