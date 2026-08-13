#include "voicelife/mcp/mcp_request_executor.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#ifndef ESP_PLATFORM
#include <thread>
#endif
#include <utility>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace voicelife::mcp {
namespace {

constexpr std::size_t kQueueCapacity = 4;
#ifdef ESP_PLATFORM
// JSON-RPC parsing, SQLite transactions and response serialization execute on
// this task. The default 6 KB task stack overflows on an actual schedule.create
// request, so keep the execution context aligned with the Runtime task budget.
constexpr uint32_t kWorkerStackBytes = 12 * 1024;
#endif

}  // namespace

class McpRequestExecutor::Impl final {
   public:
    explicit Impl(McpJsonRpcHandler handler) : handler_(std::move(handler)) {}

    ~Impl() { Stop(); }

    Status Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return Status::Ok();
        stopping_ = false;
        running_ = true;
#ifdef ESP_PLATFORM
        if (xTaskCreate(&Impl::TaskEntry, "voicelife_mcp", kWorkerStackBytes, this, 4, &task_) != pdPASS) {
            running_ = false;
            return Status::Error(ErrorCode::kInternal, "创建 MCP 工作任务失败");
        }
#else
        worker_ = std::thread(&Impl::Run, this);
#endif
        return Status::Ok();
    }

    void Stop() {
        std::deque<Request> abandoned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            stopping_ = true;
            abandoned.swap(queue_);
        }
        for (Request& request : abandoned) {
            request.response_sink(Result<std::string>::Failure(ErrorCode::kUnavailable, "MCP 服务正在停止"));
        }
        ready_.notify_all();
#ifdef ESP_PLATFORM
        for (int attempt = 0; attempt < 50 && running_.load(); ++attempt) vTaskDelay(pdMS_TO_TICKS(10));
#else
        if (worker_.joinable()) worker_.join();
#endif
    }

    Status Submit(std::string_view request, McpJsonRpcResponseSink response_sink) {
        if (!response_sink) return Status::Error(ErrorCode::kInvalidArgument, "MCP 响应回调不能为空");
        Status rejection = Status::Ok();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopping_) {
                rejection = Status::Error(ErrorCode::kUnavailable, "MCP 服务尚未就绪");
            } else if (queue_.size() >= kQueueCapacity) {
                rejection = Status::Error(ErrorCode::kUnavailable, "MCP 请求队列已满");
            } else {
                queue_.push_back({.request = std::string(request), .response_sink = std::move(response_sink)});
            }
        }
        if (!rejection.ok()) {
            response_sink(Result<std::string>::Failure(rejection.code, rejection.message));
            return rejection;
        }
        ready_.notify_one();
        return Status::Ok();
    }

   private:
    struct Request {
        std::string request;
        McpJsonRpcResponseSink response_sink;
    };

#ifdef ESP_PLATFORM
    static void TaskEntry(void* context) {
        static_cast<Impl*>(context)->Run();
        vTaskDelete(nullptr);
    }
#endif

    void Run() {
        while (true) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) break;
                request = std::move(queue_.front());
                queue_.pop_front();
            }
            request.response_sink(handler_(request.request));
        }
        running_.store(false);
    }

    McpJsonRpcHandler handler_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Request> queue_;
    std::atomic_bool running_{false};
    bool stopping_ = false;
#ifdef ESP_PLATFORM
    TaskHandle_t task_ = nullptr;
#else
    std::thread worker_;
#endif
};

McpRequestExecutor::McpRequestExecutor(McpJsonRpcHandler handler) : impl_(new Impl(std::move(handler))) {}

McpRequestExecutor::~McpRequestExecutor() { delete impl_; }

Status McpRequestExecutor::Start() { return impl_->Start(); }

void McpRequestExecutor::Stop() { impl_->Stop(); }

Status McpRequestExecutor::Submit(std::string_view request, McpJsonRpcResponseSink response_sink) {
    return impl_->Submit(request, std::move(response_sink));
}

}  // namespace voicelife::mcp
