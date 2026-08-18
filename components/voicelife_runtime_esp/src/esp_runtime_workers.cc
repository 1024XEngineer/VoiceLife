#include "esp_runtime_internal.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "im_binding_mcp_tools.h"
#include "linx_mcp_bridge.h"
#include "mcp_worker_policy.h"
#include "voicelife/im/im_retry_policy.h"

namespace voicelife::runtime {
void Runtime::StopEventLoop() {
    if (event_task_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_queue_.clear();
        event_loop_stop_ = true;
    }
    event_cv_.notify_one();
    for (int attempt = 0; attempt < 20 && !event_loop_stopped_; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    event_task_ = nullptr;
}
Status Runtime::StartMcpWorker() {
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

void Runtime::StopMcpWorker() {
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

void Runtime::StartBindingPolling(uint64_t generation) {
    if (!binding_poll_lease_.Acquire(generation)) {
        ESP_LOGI(kTag, "IM_BINDING_POLL_ADOPTED generation=%llu", static_cast<unsigned long long>(generation));
        return;
    }
    if (xTaskCreate(&Runtime::BindingPollTaskEntry, "voicelife_binding_poll", kBindingPollStackBytes, this, 2,
                    nullptr) != pdPASS) {
        if (binding_poll_lease_.Release(generation)) {
            EnqueueBindingResult(binding_use_case_.AbortPending(generation));
        }
        ESP_LOGW(kTag, "IM_BINDING_POLL_TASK_FAILED=1");
        return;
    }
    ESP_LOGI(kTag, "IM_BINDING_POLL_STARTED generation=%llu", static_cast<unsigned long long>(generation));
}

void Runtime::BindingPollTaskEntry(void* context) { static_cast<Runtime*>(context)->BindingPollLoop(); }

void Runtime::BindingPollLoop() {
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
    vTaskDelete(nullptr);
}
std::string Runtime::TruncateUtf8(std::string_view value, std::size_t max_bytes) {
    if (value.size() <= max_bytes) return std::string(value);
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) --end;
    return std::string(value.substr(0, end)) + "...";
}

bool Runtime::IsMcpToolCall(std::string_view payload) {
    JsonValue request;
    if (!ParseJson(payload, request).ok() || !request.IsObject()) return false;
    const JsonValue* method = request.Get("method");
    return method != nullptr && method->IsString() && method->string == "tools/call";
}

Result<std::string> Runtime::HandleMcpRequest(std::string_view payload, std::string_view session_id) {
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
    ESP_LOGI(kTag, "MCP_REQUEST_QUEUED bytes=%u", static_cast<unsigned>(payload.size()));
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

void Runtime::McpWorkerTaskEntry(void* arg) { static_cast<Runtime*>(arg)->McpWorkerLoop(); }

void Runtime::McpWorkerLoop() {
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
        if (tool_call && session_) session_->ReportToolCallStarted();
        auto response = HandleLinxMcpPayload(request->payload, mcp_server_, request->session_id);
        if (!response.ok()) {
            response = BuildLinxMcpUnavailableResponse(request->payload, "设备 MCP 执行失败", request->session_id);
        }
        if (tool_call && !request->abandoned.load() && session_) {
            const LinxMcpToolOutcome outcome = InspectLinxMcpToolOutcome(request->payload, response);
            session_->ReportToolResult(TruncateUtf8(outcome.summary, 96), outcome.success);
        }
        ESP_LOGI(kTag, "MCP_TOOL_EXECUTED tool_call=%d result=%d", tool_call ? 1 : 0, response.ok() ? 1 : 0);
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
void Runtime::StartImRuntime() {
#if CONFIG_VOICELIFE_IM_GATEWAY
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

void Runtime::ImLifecycleTaskEntry(void* context) { static_cast<Runtime*>(context)->ImLifecycleTask(); }

void Runtime::ImLifecycleTask() {
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
            // 选择 #235 的“重启后重新开始”策略：不恢复任何旧会话；下一次
            // 明确语音命令会创建新会话，Gateway 会原子取消同设备旧 pending。
            binding_use_case_.Bind(*im_runtime_.pairing_client(), im_pairing_clock_, im_runtime_.user_id());
            EnqueueBindingReset(binding_use_case_.generation());
            RegisterImPairingAcceptance(im_runtime_.pairing_client(), im_runtime_.device_id(), im_runtime_.user_id());
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

        ESP_LOGW(kTag, "IM_RUNTIME_DEGRADED=1 state=%d code=%d http_status=%d", static_cast<int>(im_runtime_.state()),
                 static_cast<int>(status.code), response.status_code);
        const auto delay_ms = retry_policy.NextDelay(response);
        if (!delay_ms.has_value()) break;
        ESP_LOGI(kTag, "IM_RUNTIME_RETRY attempt=%u delay_ms=%u", static_cast<unsigned>(retry_policy.attempts()),
                 static_cast<unsigned>(*delay_ms));
        vTaskDelay(pdMS_TO_TICKS(*delay_ms));
    }
    vTaskDelete(nullptr);
}
}  // namespace voicelife::runtime
#endif
