#include "voicelife/audio_esp/esp_wakenet_detector.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_models.h"
#include "model_path.h"

namespace voicelife::audio_esp {
namespace {
constexpr char kTag[] = "VoiceLifeWake";
constexpr std::size_t kMaxFrameSamples = voice::AudioFrame::kMaxPayloadBytes / sizeof(int16_t);
// A maximum-size input frame plus an incomplete model chunk must fit without
// growing a real-time buffer. The second half also lets one wrapped chunk be
// copied into fixed scratch storage.
constexpr std::size_t kInputCapacitySamples = kMaxFrameSamples * 2;

Status Failure(ErrorCode code, const char* message) { return Status::Error(code, message); }
}  // namespace

class EspWakeNetDetector::Impl final {
   public:
    explicit Impl(const void* model_root) : model_root_(model_root) {}
    ~Impl() { Reset(); }

    Status Start(WakeSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Status model_status = EnsureModelLocked();
        if (!model_status.ok()) return model_status;
        const Status buffer_status = PrepareInputLocked();
        if (!buffer_status.ok()) return buffer_status;
        sink_ = std::move(sink);
        ResetInputLocked();
        running_ = true;
        ESP_LOGI(kTag, "WAKE_DETECTOR_READY sample_rate=%d chunk_samples=%d", iface_->get_samp_rate(model_data_),
                 iface_->get_samp_chunksize(model_data_));
        return Status::Ok();
    }

    Status Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        ResetInputLocked();
        sink_ = {};
        return Status::Ok();
    }

    Status Submit(const voice::AudioFrame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_ || iface_ == nullptr || model_data_ == nullptr) {
            return Failure(ErrorCode::kUnavailable, "WakeNet 检测器未运行");
        }
        if (frame.format.codec != voice::AudioCodec::kPcmS16Le || frame.format.sample_rate_hz != 16000 ||
            frame.format.channels != 1 || frame.format.bits_per_sample != 16 || frame.payload.empty() ||
            frame.payload.size() % sizeof(int16_t) != 0) {
            return Failure(ErrorCode::kInvalidArgument, "WakeNet 仅接受 16 kHz S16LE 单声道 PCM");
        }
        const auto* samples = reinterpret_cast<const int16_t*>(frame.payload.data());
        const std::size_t sample_count = frame.payload.size() / sizeof(int16_t);
        const Status append_status = AppendInputLocked(samples, sample_count);
        if (!append_status.ok()) return append_status;
        if (chunk_samples_ <= 0) return Failure(ErrorCode::kInternal, "WakeNet 分块大小无效");
        ++frames_seen_;
        if (frames_seen_ == 1 || frames_seen_ % 500 == 0) {
            ESP_LOGI(kTag, "WAKE_INPUT_FRAMES=%llu", static_cast<unsigned long long>(frames_seen_));
        }
        while (running_ && input_available_ >= static_cast<std::size_t>(chunk_samples_)) {
            const int detected = static_cast<int>(iface_->detect(model_data_, ChunkInputLocked()));
            ConsumeChunkLocked();
            if (detected > 0) {
                const char* word = iface_->get_word_name(model_data_, detected);
                std::string name = word == nullptr ? "WakeNet" : word;
                WakeSink sink = std::move(sink_);
                running_ = false;
                ResetInputLocked();
                lock.unlock();
                ESP_LOGI(kTag, "WAKE_DETECTED model_word=%s", name.c_str());
                if (sink) sink(name);
                return Status::Ok();
            }
        }
        return Status::Ok();
    }

   private:
    Status EnsureModelLocked() {
        if (iface_ != nullptr && model_data_ != nullptr) {
            return chunk_samples_ > 0 && static_cast<std::size_t>(chunk_samples_) <= kMaxFrameSamples
                       ? Status::Ok()
                       : Failure(ErrorCode::kUnavailable, "WakeNet 模型分块大小不受支持");
        }
        if (model_root_ == nullptr) return Failure(ErrorCode::kUnavailable, "WakeNet 模型资源未就绪");
        models_ = srmodel_load(model_root_);
        if (models_ == nullptr || models_->num <= 0) return Failure(ErrorCode::kUnavailable, "WakeNet 模型加载失败");
        char* model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
        if (model_name == nullptr) return Failure(ErrorCode::kNotFound, "受控 srmodels.bin 中没有 WakeNet 模型");
        iface_ = esp_wn_handle_from_name(model_name);
        if (iface_ == nullptr) return Failure(ErrorCode::kUnavailable, "WakeNet 模型接口不可用");
        model_data_ = iface_->create(model_name, DET_MODE_95);
        if (model_data_ == nullptr) return Failure(ErrorCode::kUnavailable, "WakeNet 模型实例创建失败");
        if (iface_->get_samp_rate(model_data_) != 16000 || iface_->get_channel_num(model_data_) != 1) {
            return Failure(ErrorCode::kUnavailable, "WakeNet 模型格式与 SparkBot 16k 单声道输入不兼容");
        }
        chunk_samples_ = iface_->get_samp_chunksize(model_data_);
        if (chunk_samples_ <= 0 || static_cast<std::size_t>(chunk_samples_) > kMaxFrameSamples) {
            return Failure(ErrorCode::kUnavailable, "WakeNet 模型分块大小不受支持");
        }
        ESP_LOGI(kTag, "WAKE_MODEL_READY model=%s", model_name);
        return Status::Ok();
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
            return Failure(ErrorCode::kUnavailable, "WakeNet 实时 PCM 缓冲分配失败");
        }
        ResetInputLocked();
        return Status::Ok();
    }

    Status AppendInputLocked(const int16_t* samples, std::size_t sample_count) {
        if (input_ == nullptr || sample_count > kInputCapacitySamples - input_available_) {
            return Failure(ErrorCode::kUnavailable, "WakeNet PCM 环形缓冲不可用或已满");
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

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        ResetInputLocked();
        if (iface_ != nullptr && model_data_ != nullptr) iface_->destroy(model_data_);
        model_data_ = nullptr;
        iface_ = nullptr;
        // srmodel_load() parses the externally owned assets mmap. Match ESP-SR's
        // device cleanup path: destroy the model before releasing its index, but
        // never free or unmap the board-owned assets mapping here.
        if (models_ != nullptr) esp_srmodel_deinit(models_);
        models_ = nullptr;
        heap_caps_free(input_);
        heap_caps_free(chunk_scratch_);
        input_ = nullptr;
        chunk_scratch_ = nullptr;
        chunk_samples_ = 0;
    }

    const void* model_root_ = nullptr;
    std::mutex mutex_;
    WakeSink sink_;
    srmodel_list_t* models_ = nullptr;
    const esp_wn_iface_t* iface_ = nullptr;
    model_iface_data_t* model_data_ = nullptr;
    int16_t* input_ = nullptr;
    int16_t* chunk_scratch_ = nullptr;
    int chunk_samples_ = 0;
    std::size_t input_read_ = 0;
    std::size_t input_write_ = 0;
    std::size_t input_available_ = 0;
    uint64_t frames_seen_ = 0;
    bool running_ = false;
};

EspWakeNetDetector::EspWakeNetDetector(const void* model_root) : impl_(std::make_unique<Impl>(model_root)) {}
EspWakeNetDetector::~EspWakeNetDetector() = default;
Status EspWakeNetDetector::Start(WakeSink sink) { return impl_->Start(std::move(sink)); }
Status EspWakeNetDetector::Stop() { return impl_->Stop(); }
Status EspWakeNetDetector::Submit(const voice::AudioFrame& frame) { return impl_->Submit(frame); }

}  // namespace voicelife::audio_esp

#else

namespace voicelife::audio_esp {
class EspWakeNetDetector::Impl {};
EspWakeNetDetector::EspWakeNetDetector(const void*) : impl_(std::make_unique<Impl>()) {}
EspWakeNetDetector::~EspWakeNetDetector() = default;
Status EspWakeNetDetector::Start(WakeSink) { return Status::Error(ErrorCode::kUnavailable, "WakeNet 仅支持 ESP 平台"); }
Status EspWakeNetDetector::Stop() { return Status::Ok(); }
Status EspWakeNetDetector::Submit(const voice::AudioFrame&) {
    return Status::Error(ErrorCode::kUnavailable, "WakeNet 仅支持 ESP 平台");
}
}  // namespace voicelife::audio_esp

#endif
