#include "voicelife/audio_esp/esp_multinet_wake_detector.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

namespace voicelife::audio_esp {
namespace {

constexpr char kTag[] = "VoiceLifeWake";
constexpr char kModelPartition[] = "model";
constexpr char kModelLanguage[] = "cn";
constexpr std::size_t kMaxFrameSamples = voice::AudioFrame::kMaxPayloadBytes / sizeof(int16_t);
constexpr std::size_t kInputCapacitySamples = kMaxFrameSamples * 2;
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
    ~Impl() { ResetModel(); }

    Status Start(LocalWakeDetectorPort::WakeSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureModelLocked().ok()) return model_status_;
        const Status buffer_status = PrepareInputLocked();
        if (!buffer_status.ok()) return buffer_status;
        sink_ = std::move(sink);
        running_ = true;
        ResetInputLocked();
        multinet_->clean(model_data_);
        return Status::Ok();
    }

    Status Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        ResetInputLocked();
        if (multinet_ != nullptr && model_data_ != nullptr) multinet_->clean(model_data_);
        sink_ = {};
        return Status::Ok();
    }

    Status Submit(const voice::AudioFrame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_ || multinet_ == nullptr || model_data_ == nullptr) {
            return DetectorError(ErrorCode::kUnavailable, "本地唤醒检测器未运行");
        }
        if (frame.format.codec != voice::AudioCodec::kPcmS16Le || frame.format.sample_rate_hz != 16000 ||
            frame.format.channels != 1 || frame.format.bits_per_sample != 16 || frame.payload.empty() ||
            frame.payload.size() % sizeof(int16_t) != 0) {
            return DetectorError(ErrorCode::kInvalidArgument, "本地唤醒帧必须是 16 kHz S16LE 单声道 PCM");
        }
        const auto* samples = reinterpret_cast<const int16_t*>(frame.payload.data());
        const std::size_t sample_count = frame.payload.size() / sizeof(int16_t);
        const Status append_status = AppendInputLocked(samples, sample_count);
        if (!append_status.ok()) return append_status;
        if (chunk_samples_ <= 0) return DetectorError(ErrorCode::kUnavailable, "MultiNet 分块大小无效");
        while (input_available_ >= static_cast<std::size_t>(chunk_samples_) && running_) {
            const esp_mn_state_t state = multinet_->detect(model_data_, ChunkInputLocked());
            ConsumeChunkLocked();
            if (state == ESP_MN_STATE_DETECTED) {
                const esp_mn_results_t* result = multinet_->get_results(model_data_);
                if (result != nullptr) {
                    for (int i = 0; i < result->num; ++i) {
                        const int command_id = result->command_id[i];
                        const LocalCommand* matched = nullptr;
                        for (const auto& command : kCommands) {
                            if (command.id == command_id) {
                                matched = &command;
                                break;
                            }
                        }
                        if (matched != nullptr) {
                            running_ = false;
                            WakeSink sink = sink_;
                            ResetInputLocked();
                            multinet_->clean(model_data_);
                            sink_ = {};
                            lock.unlock();
                            if (sink) sink(matched->display);
                            return Status::Ok();
                        }
                    }
                }
                multinet_->clean(model_data_);
            } else if (state == ESP_MN_STATE_TIMEOUT) {
                multinet_->clean(model_data_);
            }
        }
        return Status::Ok();
    }

   private:
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
        // 对齐小智 CustomWakeWord 的默认阈值。MultiNet 命令词并非 WakeNet
        // 唤醒模型；0.5 会让短词“牛牛”在实际近讲场景明显漏检。
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
        // ESP-SR's MultiNet implementation returns -1 after applying this
        // setting on ESP32-S3. Xiaozhi's CustomWakeWord uses the same API as
        // a setter and intentionally does not treat its return value as an
        // esp_err_t. On this SDK, commands_update also returns a non-null
        // report with num == 0 on success; only entries in that report are
        // command compilation failures.
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

    void ResetModel() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
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

    std::mutex mutex_;
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
    bool running_ = false;
};

EspMultiNetWakeDetector::EspMultiNetWakeDetector() : impl_(std::make_unique<Impl>()) {}
EspMultiNetWakeDetector::~EspMultiNetWakeDetector() = default;
Status EspMultiNetWakeDetector::Start(WakeSink sink) { return impl_->Start(std::move(sink)); }
Status EspMultiNetWakeDetector::Stop() { return impl_->Stop(); }
Status EspMultiNetWakeDetector::Submit(const voice::AudioFrame& frame) { return impl_->Submit(frame); }

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
Status EspMultiNetWakeDetector::Submit(const voice::AudioFrame&) {
    return Status::Error(ErrorCode::kUnavailable, "ESP-SR 仅支持 ESP 平台");
}
}  // namespace voicelife::audio_esp

#endif
