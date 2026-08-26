#include "voicelife/audio_esp/esp_opus_codec_strategy.h"

#include <array>
#include <memory>
#include <mutex>
#include <utility>

#include "esp_audio_types.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "voicelife/audio_esp/sparkbot_audio_budget.h"

namespace voicelife::audio_esp {
namespace {

constexpr uint32_t kSampleRateHz = 16000;
constexpr uint8_t kChannels = 1;
constexpr uint8_t kBitsPerSample = 16;
constexpr uint16_t kFrameDurationMs = kSparkBotOpusFrameDurationMs;
constexpr std::size_t kPcmFrameBytes = kSampleRateHz * kFrameDurationMs / 1000U * kChannels * (kBitsPerSample / 8U);
constexpr std::size_t kMaxOpusPacketBytes = 1275;
constexpr std::size_t kEncodePoolSlots = 20;
constexpr std::size_t kDecodePoolLogStep = 8;
constexpr char kTag[] = "voicelife_opus";

bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right) {
    return left.codec == right.codec && left.sample_rate_hz == right.sample_rate_hz &&
           left.channels == right.channels && left.bits_per_sample == right.bits_per_sample &&
           left.frame_duration_ms == right.frame_duration_ms;
}

bool IsSupportedLocalPcm(const voice::AudioFormat& format) {
    return format.codec == voice::AudioCodec::kPcmS16Le && format.sample_rate_hz == kSampleRateHz &&
           format.channels == kChannels && format.bits_per_sample == kBitsPerSample &&
           format.frame_duration_ms == kFrameDurationMs;
}

bool IsSupportedWireOpus(const voice::AudioFormat& format) {
    return format.codec == voice::AudioCodec::kOpus && format.sample_rate_hz == kSampleRateHz &&
           format.channels == kChannels && format.bits_per_sample == kBitsPerSample &&
           format.frame_duration_ms == kFrameDurationMs;
}

Status CodecError(std::string message, int error) {
    message += " err=" + std::to_string(error);
    return Status::Error(ErrorCode::kUnavailable, std::move(message));
}

class EspOpusCodecStrategy final : public voice::CodecStrategy {
   public:
    ~EspOpusCodecStrategy() override { Reset(); }

    [[nodiscard]] voice::AudioCodec codec() const override { return voice::AudioCodec::kOpus; }

    Status Configure(const voice::AudioFormat& local_pcm, const voice::AudioFormat& wire) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsSupportedLocalPcm(local_pcm) || !IsSupportedWireOpus(wire)) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "SparkBot Opus 只支持 PCM/Opus 16kHz mono S16LE 20ms Profile");
        }
        if (encoder_ != nullptr && decoder_ != nullptr && SameFormat(local_pcm_, local_pcm) &&
            SameFormat(wire_, wire)) {
            return Status::Ok();
        }
        ResetLocked();

        esp_opus_enc_config_t encoder_config = {
            .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
            .channel = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .bitrate = 16000,
            .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
            // Keep the real-time uplink encoder at the low-complexity setting
            // used by the SparkBot reference. The synchronous encoder runs in
            // the audio delivery task; complexity 10 can starve IDLE1 and
            // trigger the ESP task watchdog during continuous speech.
            .complexity = 0,
            .enable_fec = false,
            .enable_dtx = false,
            .enable_vbr = false,
        };
        const int encoder_status = esp_opus_enc_open(&encoder_config, sizeof(encoder_config), &encoder_);
        if (encoder_status != ESP_AUDIO_ERR_OK || encoder_ == nullptr) {
            ResetLocked();
            return CodecError("创建 Opus 编码器失败", encoder_status);
        }
        int input_bytes = 0;
        int output_bytes = 0;
        const int frame_status = esp_opus_enc_get_frame_size(encoder_, &input_bytes, &output_bytes);
        if (frame_status != ESP_AUDIO_ERR_OK || input_bytes != static_cast<int>(kPcmFrameBytes) || output_bytes <= 0 ||
            output_bytes > static_cast<int>(kMaxOpusPacketBytes)) {
            ResetLocked();
            return CodecError("Opus 编码器帧预算与 20ms Profile 不匹配", frame_status);
        }

        esp_opus_dec_cfg_t decoder_config = {
            .sample_rate = kSampleRateHz,
            .channel = ESP_AUDIO_MONO,
            .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
            .self_delimited = false,
        };
        const int decoder_status = esp_opus_dec_open(&decoder_config, sizeof(decoder_config), &decoder_);
        if (decoder_status != ESP_AUDIO_ERR_OK || decoder_ == nullptr) {
            ResetLocked();
            return CodecError("创建 Opus 解码器失败", decoder_status);
        }

        encoded_pool_ = voice::AudioPayloadPool::Create(kEncodePoolSlots, kMaxOpusPacketBytes);
        decoded_pool_ = voice::AudioPayloadPool::Create(kSparkBotOpusDecodePoolSlots, kPcmFrameBytes);
        if (encoded_pool_ == nullptr || decoded_pool_ == nullptr) {
            ResetLocked();
            return Status::Error(ErrorCode::kUnavailable, "创建 Opus 有界负载池失败");
        }
        local_pcm_ = local_pcm;
        wire_ = wire;
        ESP_LOGI(kTag, "OPUS_READY sample_rate=%u frame_ms=%u bitrate=16000 cbr=1 tx_slots=%u rx_slots=%u",
                 static_cast<unsigned>(wire.sample_rate_hz), static_cast<unsigned>(wire.frame_duration_ms),
                 static_cast<unsigned>(kEncodePoolSlots), static_cast<unsigned>(kSparkBotOpusDecodePoolSlots));
        return Status::Ok();
    }

    Result<voice::AudioFrame> Encode(const voice::AudioFrame& pcm) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (encoder_ == nullptr || encoded_pool_ == nullptr || !SameFormat(pcm.format, local_pcm_) ||
            pcm.payload.size() != kPcmFrameBytes) {
            return Result<voice::AudioFrame>::Failure(ErrorCode::kInvalidArgument,
                                                      "Opus 编码输入不是已配置的 PCM 20ms 帧");
        }
        voice::AudioPayload payload = encoded_pool_->TryAcquire();
        if (payload.empty()) {
            return Result<voice::AudioFrame>::Failure(ErrorCode::kUnavailable, "Opus 上行负载池已满");
        }
        esp_audio_enc_in_frame_t input = {
            .buffer = const_cast<uint8_t*>(pcm.payload.data()),
            .len = static_cast<uint32_t>(pcm.payload.size()),
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = payload.data(),
            .len = static_cast<uint32_t>(payload.size()),
            .encoded_bytes = 0,
            .pts = 0,
        };
        const int status = esp_opus_enc_process(encoder_, &input, &output);
        if (status != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0 || output.encoded_bytes > payload.size()) {
            return Result<voice::AudioFrame>::Failure(ErrorCode::kUnavailable,
                                                      "Opus 上行编码失败 err=" + std::to_string(status));
        }
        payload.resize(output.encoded_bytes);
        voice::AudioFrame encoded;
        encoded.generation = pcm.generation;
        encoded.sequence = pcm.sequence;
        encoded.format = wire_;
        encoded.payload = std::move(payload);
        return Result<voice::AudioFrame>::Success(std::move(encoded));
    }

    Result<voice::AudioFrame> Decode(const voice::AudioFrame& encoded) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (decoder_ == nullptr || decoded_pool_ == nullptr || !SameFormat(encoded.format, wire_) ||
            encoded.payload.empty() || encoded.payload.size() > kMaxOpusPacketBytes) {
            return Result<voice::AudioFrame>::Failure(ErrorCode::kInvalidArgument, "Opus 解码输入不是已配置的线上帧");
        }
        voice::AudioPayload payload = decoded_pool_->TryAcquire();
        if (payload.empty()) {
            LogDecodePoolLocked("exhausted");
            return Result<voice::AudioFrame>::Failure(ErrorCode::kConflict, "Opus 下行负载池已满");
        }
        MaybeLogDecodePoolLocked();
        esp_audio_dec_in_raw_t input = {
            .buffer = const_cast<uint8_t*>(encoded.payload.data()),
            .len = static_cast<uint32_t>(encoded.payload.size()),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = payload.data(),
            .len = static_cast<uint32_t>(payload.size()),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};
        const int status = esp_opus_dec_decode(decoder_, &input, &output, &info);
        if (status != ESP_AUDIO_ERR_OK || input.consumed != encoded.payload.size() ||
            output.decoded_size != kPcmFrameBytes) {
            return Result<voice::AudioFrame>::Failure(ErrorCode::kUnavailable,
                                                      "Opus 下行解码失败 err=" + std::to_string(status));
        }
        payload.resize(output.decoded_size);
        voice::AudioFrame pcm;
        pcm.generation = encoded.generation;
        pcm.sequence = encoded.sequence;
        pcm.format = local_pcm_;
        pcm.payload = std::move(payload);
        return Result<voice::AudioFrame>::Success(std::move(pcm));
    }

   private:
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetLocked();
    }

    void ResetLocked() {
        encoded_pool_.reset();
        decoded_pool_.reset();
        if (encoder_ != nullptr) {
            esp_opus_enc_close(encoder_);
            encoder_ = nullptr;
        }
        if (decoder_ != nullptr) {
            esp_opus_dec_close(decoder_);
            decoder_ = nullptr;
        }
        local_pcm_ = {};
        wire_ = {};
        logged_decode_pool_high_watermark_ = 0;
    }

    void MaybeLogDecodePoolLocked() {
        if (decoded_pool_ == nullptr) return;
        const std::size_t high_watermark = decoded_pool_->high_watermark();
        if (high_watermark < logged_decode_pool_high_watermark_ + kDecodePoolLogStep) return;
        logged_decode_pool_high_watermark_ = high_watermark;
        LogDecodePoolLocked("watermark");
    }

    void LogDecodePoolLocked(const char* event) const {
        if (decoded_pool_ == nullptr) return;
        ESP_LOGI(kTag, "OPUS_RX_POOL event=%s slots=%u in_use=%u high_watermark=%u acquisition_failures=%u", event,
                 static_cast<unsigned>(decoded_pool_->slot_count()), static_cast<unsigned>(decoded_pool_->in_use()),
                 static_cast<unsigned>(decoded_pool_->high_watermark()),
                 static_cast<unsigned>(decoded_pool_->acquisition_failures()));
    }

    std::mutex mutex_;
    void* encoder_ = nullptr;
    void* decoder_ = nullptr;
    voice::AudioFormat local_pcm_;
    voice::AudioFormat wire_;
    std::shared_ptr<voice::AudioPayloadPool> encoded_pool_;
    std::shared_ptr<voice::AudioPayloadPool> decoded_pool_;
    std::size_t logged_decode_pool_high_watermark_ = 0;
};

}  // namespace

std::unique_ptr<voice::CodecStrategy> CreateEspOpusCodecStrategy() { return std::make_unique<EspOpusCodecStrategy>(); }

}  // namespace voicelife::audio_esp
