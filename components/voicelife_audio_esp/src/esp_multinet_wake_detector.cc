#include "voicelife/audio_esp/esp_multinet_wake_detector.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

namespace voicelife::audio_esp {
namespace {

constexpr char kTag[] = "VoiceLifeWake";
constexpr char kModelPartition[] = "model";
constexpr char kModelLanguage[] = "cn";
constexpr std::size_t kMaxFrameSamples = voice::AudioFrame::kMaxPayloadBytes / sizeof(int16_t);
constexpr std::size_t kInputCapacitySamples = kMaxFrameSamples * 2;
constexpr std::size_t kMailboxCapacity = 9;
constexpr uint32_t kWorkerStackWords = 16384 / sizeof(StackType_t);
constexpr UBaseType_t kWorkerPriority = 4;

struct LocalCommand {
    int id;
    const char* grammar;
    const char* display;
};

// MultiNet's command grammar is a pinyin token sequence. These commands are
// registered once per active board assembly, not from Runtime.
constexpr LocalCommand kCommands[] = {
    {1, "ni hao niu niu", "你好牛牛"},
    {2, "niu niu", "牛牛"},
    {3, "bie shuo le", "别说了"},
};

Status DetectorError(ErrorCode code, const char* message) { return Status::Error(code, message); }

}  // namespace

class EspMultiNetWakeDetector::Impl final {
   public:
    ~Impl() {
        (void)Stop();
        ShutdownWorker();
        ResetModel();
    }

    Status Start(LocalWakeDetectorPort::WakeSink sink) {
        {
            std::lock_guard<std::mutex> model_lock(model_mutex_);
            if (!EnsureModelLocked().ok()) return model_status_;
            const Status buffer_status = PrepareInputLocked();
            if (!buffer_status.ok()) return buffer_status;
            const Status worker_status = EnsureWorkerLocked();
            if (!worker_status.ok()) return worker_status;
            multinet_->clean(model_data_);
            sink_ = std::move(sink);
        }
        const uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            ClearMailboxLocked();
        }
        running_.store(true, std::memory_order_release);
        ESP_LOGI(kTag, "WAKE_WORKER_ARMED generation=%llu", static_cast<unsigned long long>(generation));
        return Status::Ok();
    }

    Status Stop() {
        // Advance the token before acquiring either mutex. A worker that has
        // already dequeued a frame will reject it both before and after model
        // inference, so a stop/start cannot leak an old wake callback.
        const uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            ClearMailboxLocked();
        }
        {
            std::lock_guard<std::mutex> model_lock(model_mutex_);
            sink_ = {};
            ResetInputLocked();
            if (multinet_ != nullptr && model_data_ != nullptr) multinet_->clean(model_data_);
        }
        ESP_LOGI(kTag, "WAKE_WORKER_DISARMED generation=%llu mailbox_drops=%llu",
                 static_cast<unsigned long long>(generation),
                 static_cast<unsigned long long>(mailbox_drops_.load(std::memory_order_relaxed)));
        return Status::Ok();
    }

    Status Submit(voice::AudioFrame frame) {
        if (!running_.load(std::memory_order_acquire)) {
            return DetectorError(ErrorCode::kUnavailable, "本地唤醒检测器未运行");
        }
        if (!IsValidFrame(frame)) {
            return DetectorError(ErrorCode::kInvalidArgument, "本地唤醒帧必须是 16 kHz S16LE 单声道 PCM");
        }

        // This is the only work performed in voice_audio_sink. In particular,
        // it never takes model_mutex_ or invokes ESP-SR inference.
        const uint64_t generation = generation_.load(std::memory_order_acquire);
        bool dropped_oldest = false;
        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            if (!running_.load(std::memory_order_acquire) ||
                generation != generation_.load(std::memory_order_acquire)) {
                return DetectorError(ErrorCode::kUnavailable, "本地唤醒检测器未运行");
            }
            if (mailbox_size_ == kMailboxCapacity) {
                mailbox_[mailbox_head_] = {};
                mailbox_head_ = (mailbox_head_ + 1) % kMailboxCapacity;
                --mailbox_size_;
                dropped_oldest = true;
            }
            const std::size_t tail = (mailbox_head_ + mailbox_size_) % kMailboxCapacity;
            mailbox_[tail] = PendingFrame{generation, std::move(frame)};
            ++mailbox_size_;
        }
        if (dropped_oldest) {
            const uint64_t drops = mailbox_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
            ESP_LOGW(kTag, "WAKE_MAILBOX_DROP count=%llu policy=drop_oldest", static_cast<unsigned long long>(drops));
        }
        if (worker_wakeup_ != nullptr) xSemaphoreGive(worker_wakeup_);
        return Status::Ok();
    }

   private:
    struct PendingFrame {
        uint64_t generation = 0;
        voice::AudioFrame frame;
    };

    static void WorkerEntry(void* argument) {
        static_cast<Impl*>(argument)->WorkerLoop();
        vTaskDelete(nullptr);
    }

    void WorkerLoop() {
        while (!worker_shutdown_.load(std::memory_order_acquire)) {
            if (worker_wakeup_ == nullptr || xSemaphoreTake(worker_wakeup_, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            PendingFrame pending;
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                if (mailbox_size_ == 0) continue;
                pending = std::move(mailbox_[mailbox_head_]);
                mailbox_[mailbox_head_] = {};
                mailbox_head_ = (mailbox_head_ + 1) % kMailboxCapacity;
                --mailbox_size_;
            }
            ProcessFrame(std::move(pending));
        }
        if (worker_stopped_ != nullptr) xSemaphoreGive(worker_stopped_);
    }

    void ProcessFrame(PendingFrame pending) {
        const uint64_t generation = generation_.load(std::memory_order_acquire);
        if (!running_.load(std::memory_order_acquire) || pending.generation != generation) return;

        LocalWakeDetectorPort::WakeSink matched_sink;
        const char* matched_display = nullptr;
        {
            std::lock_guard<std::mutex> model_lock(model_mutex_);
            if (!running_.load(std::memory_order_acquire) ||
                pending.generation != generation_.load(std::memory_order_acquire) || multinet_ == nullptr ||
                model_data_ == nullptr) {
                return;
            }
            const auto* samples = reinterpret_cast<const int16_t*>(pending.frame.payload.data());
            const std::size_t sample_count = pending.frame.payload.size() / sizeof(int16_t);
            if (!AppendInputLocked(samples, sample_count).ok()) {
                // This is a model-side continuity loss, not a physical PCM
                // loss. Clear its private staging buffer and resume from the
                // newest frame; the delivery path remains nonblocking.
                ResetInputLocked();
                ESP_LOGW(kTag, "WAKE_MODEL_BUFFER_RESET samples=%u", static_cast<unsigned>(sample_count));
                return;
            }
            while (input_available_ >= static_cast<std::size_t>(chunk_samples_) &&
                   running_.load(std::memory_order_acquire) &&
                   pending.generation == generation_.load(std::memory_order_acquire)) {
                const esp_mn_state_t state = multinet_->detect(model_data_, ChunkInputLocked());
                ConsumeChunkLocked();
                if (state == ESP_MN_STATE_DETECTED) {
                    const esp_mn_results_t* result = multinet_->get_results(model_data_);
                    if (result != nullptr) {
                        for (int i = 0; i < result->num; ++i) {
                            for (const auto& command : kCommands) {
                                if (command.id == result->command_id[i]) {
                                    matched_display = command.display;
                                    break;
                                }
                            }
                            if (matched_display != nullptr) break;
                        }
                    }
                    multinet_->clean(model_data_);
                    if (matched_display != nullptr && running_.load(std::memory_order_acquire) &&
                        pending.generation == generation_.load(std::memory_order_acquire)) {
                        running_.store(false, std::memory_order_release);
                        matched_sink = std::move(sink_);
                        sink_ = {};
                        ResetInputLocked();
                    }
                    break;
                }
                if (state == ESP_MN_STATE_TIMEOUT) multinet_->clean(model_data_);
            }
        }
        if (matched_sink && matched_display != nullptr) {
            ESP_LOGI(kTag, "WAKE_DETECTED word=%s", matched_display);
            matched_sink(matched_display);
        }
    }

    static bool IsValidFrame(const voice::AudioFrame& frame) {
        return frame.format.codec == voice::AudioCodec::kPcmS16Le && frame.format.sample_rate_hz == 16000 &&
               frame.format.channels == 1 && frame.format.bits_per_sample == 16 && !frame.payload.empty() &&
               frame.payload.size() % sizeof(int16_t) == 0;
    }

    Status EnsureWorkerLocked() {
        if (worker_task_ != nullptr) return Status::Ok();
        worker_shutdown_.store(false, std::memory_order_release);
        worker_wakeup_ = xSemaphoreCreateCounting(kMailboxCapacity, 0);
        worker_stopped_ = xSemaphoreCreateBinary();
        worker_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(sizeof(StackType_t) * kWorkerStackWords, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        worker_tcb_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (worker_wakeup_ == nullptr || worker_stopped_ == nullptr || worker_stack_ == nullptr ||
            worker_tcb_ == nullptr) {
            CleanupWorkerStorage();
            return DetectorError(ErrorCode::kUnavailable, "MultiNet 异步检测任务资源分配失败");
        }
        worker_task_ = xTaskCreateStatic(&WorkerEntry, "voice_wake_mn", kWorkerStackWords, this, kWorkerPriority,
                                         worker_stack_, worker_tcb_);
        if (worker_task_ == nullptr) {
            CleanupWorkerStorage();
            return DetectorError(ErrorCode::kUnavailable, "创建 MultiNet 异步检测任务失败");
        }
        return Status::Ok();
    }

    void ShutdownWorker() {
        if (worker_task_ == nullptr) return;
        worker_shutdown_.store(true, std::memory_order_release);
        if (worker_wakeup_ != nullptr) xSemaphoreGive(worker_wakeup_);
        if (worker_stopped_ == nullptr || xSemaphoreTake(worker_stopped_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(kTag, "等待 MultiNet 异步检测任务退出超时，保留任务资源避免悬空访问");
            return;
        }
        worker_task_ = nullptr;
        // FreeRTOS deletes a static task from its own context after this
        // acknowledgement. Keep the static stack/TCB and semaphores until
        // process teardown: releasing them here could race that final delete.
    }

    void CleanupWorkerStorage() {
        if (worker_wakeup_ != nullptr) {
            vSemaphoreDelete(worker_wakeup_);
            worker_wakeup_ = nullptr;
        }
        if (worker_stopped_ != nullptr) {
            vSemaphoreDelete(worker_stopped_);
            worker_stopped_ = nullptr;
        }
        heap_caps_free(worker_stack_);
        heap_caps_free(worker_tcb_);
        worker_stack_ = nullptr;
        worker_tcb_ = nullptr;
    }

    Status EnsureModelLocked() {
        if (model_status_.ok() && models_ != nullptr && multinet_ != nullptr && model_data_ != nullptr) {
            return Status::Ok();
        }
        if (!model_status_.ok()) return model_status_;
        models_ = esp_srmodel_init(kModelPartition);
        if (models_ == nullptr || models_->num <= 0) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "ESP-SR model 分区加载失败");
            return model_status_;
        }
        char* model_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, kModelLanguage);
        if (model_name == nullptr) model_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, nullptr);
        if (model_name == nullptr) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "ESP-SR 中文 MultiNet 模型不存在");
            return model_status_;
        }
        multinet_ = esp_mn_handle_from_name(model_name);
        if (multinet_ == nullptr) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "MultiNet 模型句柄创建失败");
            return model_status_;
        }
        model_data_ = multinet_->create(model_name, 3000);
        if (model_data_ == nullptr) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "MultiNet 模型实例创建失败");
            return model_status_;
        }
        chunk_samples_ = multinet_->get_samp_chunksize(model_data_);
        if (chunk_samples_ <= 0 || static_cast<std::size_t>(chunk_samples_) > kMaxFrameSamples) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "MultiNet 模型分块大小不受支持");
            return model_status_;
        }
        const int threshold_status = multinet_->set_det_threshold(model_data_, 0.2f);
        const esp_err_t alloc_status = esp_mn_commands_alloc(multinet_, model_data_);
        const esp_err_t clear_status = esp_mn_commands_clear();
        esp_err_t add_status = ESP_OK;
        for (const auto& command : kCommands) {
            if (add_status != ESP_OK) break;
            add_status = esp_mn_commands_add(command.id, command.grammar);
        }
        esp_mn_error_t* update_error = esp_mn_commands_update();
        ESP_LOGI(kTag, "WAKE_COMMAND_STATUS threshold=%d alloc=%d clear=%d add=%d update_errors=%d", threshold_status,
                 static_cast<int>(alloc_status), static_cast<int>(clear_status), static_cast<int>(add_status),
                 update_error == nullptr ? 0 : static_cast<int>(update_error->num));
        const bool command_update_failed = update_error != nullptr && update_error->num > 0;
        if (alloc_status != ESP_OK || clear_status != ESP_OK || add_status != ESP_OK || command_update_failed) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "MultiNet 唤醒命令注册失败");
            return model_status_;
        }
        ESP_LOGI(kTag, "本地命令检测器已就绪：MultiNet=%s commands=你好牛牛,牛牛,别说了", model_name);
        model_status_ = Status::Ok();
        return model_status_;
    }

    Status PrepareInputLocked() {
        if (input_ != nullptr && chunk_scratch_ != nullptr) return Status::Ok();
        input_ = static_cast<int16_t*>(
            heap_caps_malloc(kInputCapacitySamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        chunk_scratch_ = static_cast<int16_t*>(heap_caps_malloc(
            static_cast<std::size_t>(chunk_samples_) * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (input_ == nullptr || chunk_scratch_ == nullptr) {
            heap_caps_free(input_);
            heap_caps_free(chunk_scratch_);
            input_ = nullptr;
            chunk_scratch_ = nullptr;
            return DetectorError(ErrorCode::kUnavailable, "MultiNet 实时 PCM 缓冲分配失败");
        }
        ResetInputLocked();
        return Status::Ok();
    }

    Status AppendInputLocked(const int16_t* samples, std::size_t sample_count) {
        if (input_ == nullptr || sample_count > kInputCapacitySamples - input_available_) {
            return DetectorError(ErrorCode::kUnavailable, "MultiNet PCM 环形缓冲不可用或已满");
        }
        const std::size_t first = std::min(sample_count, kInputCapacitySamples - input_write_);
        std::memcpy(input_ + input_write_, samples, first * sizeof(int16_t));
        std::memcpy(input_, samples + first, (sample_count - first) * sizeof(int16_t));
        input_write_ = (input_write_ + sample_count) % kInputCapacitySamples;
        input_available_ += sample_count;
        return Status::Ok();
    }

    int16_t* ChunkInputLocked() {
        if (input_read_ + static_cast<std::size_t>(chunk_samples_) <= kInputCapacitySamples)
            return input_ + input_read_;
        const std::size_t first = kInputCapacitySamples - input_read_;
        std::memcpy(chunk_scratch_, input_ + input_read_, first * sizeof(int16_t));
        std::memcpy(chunk_scratch_ + first, input_,
                    (static_cast<std::size_t>(chunk_samples_) - first) * sizeof(int16_t));
        return chunk_scratch_;
    }

    void ConsumeChunkLocked() {
        input_read_ = (input_read_ + static_cast<std::size_t>(chunk_samples_)) % kInputCapacitySamples;
        input_available_ -= static_cast<std::size_t>(chunk_samples_);
    }

    void ResetInputLocked() {
        input_read_ = 0;
        input_write_ = 0;
        input_available_ = 0;
    }

    void ClearMailboxLocked() {
        for (auto& pending : mailbox_) pending = {};
        mailbox_head_ = 0;
        mailbox_size_ = 0;
    }

    void ResetModel() {
        std::lock_guard<std::mutex> model_lock(model_mutex_);
        sink_ = {};
        ResetInputLocked();
        (void)esp_mn_commands_free();
        if (multinet_ != nullptr && model_data_ != nullptr) multinet_->destroy(model_data_);
        model_data_ = nullptr;
        multinet_ = nullptr;
        if (models_ != nullptr) esp_srmodel_deinit(models_);
        models_ = nullptr;
        heap_caps_free(input_);
        heap_caps_free(chunk_scratch_);
        input_ = nullptr;
        chunk_scratch_ = nullptr;
        chunk_samples_ = 0;
    }

    std::mutex queue_mutex_;
    std::mutex model_mutex_;
    std::array<PendingFrame, kMailboxCapacity> mailbox_{};
    std::size_t mailbox_head_ = 0;
    std::size_t mailbox_size_ = 0;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> mailbox_drops_{0};
    std::atomic_bool running_{false};
    std::atomic_bool worker_shutdown_{false};
    LocalWakeDetectorPort::WakeSink sink_;
    srmodel_list_t* models_ = nullptr;
    esp_mn_iface_t* multinet_ = nullptr;
    model_iface_data_t* model_data_ = nullptr;
    int16_t* input_ = nullptr;
    int16_t* chunk_scratch_ = nullptr;
    int chunk_samples_ = 0;
    std::size_t input_read_ = 0;
    std::size_t input_write_ = 0;
    std::size_t input_available_ = 0;
    Status model_status_ = Status::Ok();
    SemaphoreHandle_t worker_wakeup_ = nullptr;
    SemaphoreHandle_t worker_stopped_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    StackType_t* worker_stack_ = nullptr;
    StaticTask_t* worker_tcb_ = nullptr;
};

EspMultiNetWakeDetector::EspMultiNetWakeDetector() : impl_(std::make_unique<Impl>()) {}
EspMultiNetWakeDetector::~EspMultiNetWakeDetector() = default;
Status EspMultiNetWakeDetector::Start(WakeSink sink) { return impl_->Start(std::move(sink)); }
Status EspMultiNetWakeDetector::Stop() { return impl_->Stop(); }
Status EspMultiNetWakeDetector::Submit(voice::AudioFrame frame) { return impl_->Submit(std::move(frame)); }

}  // namespace voicelife::audio_esp

#else

namespace voicelife::audio_esp {
class EspMultiNetWakeDetector::Impl {};
EspMultiNetWakeDetector::EspMultiNetWakeDetector() : impl_(std::make_unique<Impl>()) {}
EspMultiNetWakeDetector::~EspMultiNetWakeDetector() = default;
Status EspMultiNetWakeDetector::Start(WakeSink) {
    return Status::Error(ErrorCode::kUnavailable, "ESP-SR 仅支持 ESP 平台");
}
Status EspMultiNetWakeDetector::Stop() { return Status::Ok(); }
Status EspMultiNetWakeDetector::Submit(voice::AudioFrame) {
    return Status::Error(ErrorCode::kUnavailable, "ESP-SR 仅支持 ESP 平台");
}
}  // namespace voicelife::audio_esp

#endif
