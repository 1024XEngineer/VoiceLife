#include "voicelife/audio_esp/esp_multinet_wake_detector.h"

#ifdef ESP_PLATFORM

#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
constexpr char kWakeCommand[] = "ni hao niu niu";
constexpr char kWakeDisplay[] = "你好牛牛";

Status DetectorError(ErrorCode code, const char* message) { return Status::Error(code, message); }

}  // namespace

class EspMultiNetWakeDetector::Impl final {
   public:
    ~Impl() { ResetModel(); }

    Status Start(LocalWakeDetectorPort::WakeSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureModelLocked().ok()) return model_status_;
        sink_ = std::move(sink);
        running_ = true;
        input_.clear();
        multinet_->clean(model_data_);
        return Status::Ok();
    }

    Status Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        input_.clear();
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
        input_.insert(input_.end(), samples, samples + frame.payload.size() / sizeof(int16_t));
        const int chunk_size = multinet_->get_samp_chunksize(model_data_);
        if (chunk_size <= 0) return DetectorError(ErrorCode::kUnavailable, "MultiNet 分块大小无效");
        while (input_.size() >= static_cast<size_t>(chunk_size) && running_) {
            const esp_mn_state_t state = multinet_->detect(model_data_, input_.data());
            if (state == ESP_MN_STATE_DETECTED) {
                const esp_mn_results_t* result = multinet_->get_results(model_data_);
                if (result != nullptr) {
                    for (int i = 0; i < result->num; ++i) {
                        if (result->command_id[i] == 1) {
                            running_ = false;
                            WakeSink sink = sink_;
                            input_.clear();
                            multinet_->clean(model_data_);
                            sink_ = {};
                            lock.unlock();
                            if (sink) sink(kWakeDisplay);
                            return Status::Ok();
                        }
                    }
                }
                multinet_->clean(model_data_);
            } else if (state == ESP_MN_STATE_TIMEOUT) {
                multinet_->clean(model_data_);
            }
            if (input_.size() >= static_cast<size_t>(chunk_size)) {
                input_.erase(input_.begin(), input_.begin() + chunk_size);
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
        const int threshold_status = multinet_->set_det_threshold(model_data_, 0.5f);
        const esp_err_t alloc_status = esp_mn_commands_alloc(multinet_, model_data_);
        const esp_err_t clear_status = esp_mn_commands_clear();
        const esp_err_t add_status = esp_mn_commands_add(1, kWakeCommand);
        esp_mn_error_t* update_error = esp_mn_commands_update();
        ESP_LOGI(kTag, "WAKE_COMMAND_STATUS threshold=%d alloc=%d clear=%d add=%d update_errors=%d", threshold_status,
                 static_cast<int>(alloc_status), static_cast<int>(clear_status), static_cast<int>(add_status),
                 update_error == nullptr ? 0 : static_cast<int>(update_error->num));
        if (threshold_status < 0 || alloc_status != ESP_OK || clear_status != ESP_OK || add_status != ESP_OK ||
            update_error != nullptr) {
            model_status_ = DetectorError(ErrorCode::kUnavailable, "MultiNet 唤醒命令注册失败");
            return model_status_;
        }
        ESP_LOGI(kTag, "本地唤醒检测器已就绪：MultiNet=%s command=%s", model_name, kWakeDisplay);
        model_status_ = Status::Ok();
        return model_status_;
    }

    void ResetModel() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        input_.clear();
        (void)esp_mn_commands_free();
        if (multinet_ != nullptr && model_data_ != nullptr) multinet_->destroy(model_data_);
        model_data_ = nullptr;
        multinet_ = nullptr;
        if (models_ != nullptr) esp_srmodel_deinit(models_);
        models_ = nullptr;
    }

    std::mutex mutex_;
    LocalWakeDetectorPort::WakeSink sink_;
    srmodel_list_t* models_ = nullptr;
    esp_mn_iface_t* multinet_ = nullptr;
    model_iface_data_t* model_data_ = nullptr;
    std::vector<int16_t> input_;
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
