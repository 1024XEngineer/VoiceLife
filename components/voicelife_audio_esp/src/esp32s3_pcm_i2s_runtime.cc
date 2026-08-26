#include "es8311_codec_control.h"
#include "esp32s3_pcm_audio_port_internal.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esp_err.h"
#include "esp_log.h"

namespace voicelife::audio_esp {

namespace {

uint16_t AbsolutePcm16(int16_t sample) {
    const int32_t value = sample;
    return static_cast<uint16_t>(value < 0 ? -value : value);
}

void RaisePeak(std::atomic<uint16_t>& peak, uint16_t observed) {
    uint16_t current = peak.load();
    while (observed > current && !peak.compare_exchange_weak(current, observed)) {
    }
}

}  // namespace

namespace detail {

bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right, bool include_frame_duration) {
    return left.codec == right.codec && left.sample_rate_hz == right.sample_rate_hz &&
           left.channels == right.channels && left.bits_per_sample == right.bits_per_sample &&
           (!include_frame_duration || left.frame_duration_ms == right.frame_duration_ms);
}

size_t WireBytes(const I2sEndpointProfile& endpoint) { return endpoint.wire_bits_per_sample / 8U; }

namespace {
uint8_t WireSlotCount(const I2sEndpointProfile& endpoint) {
    return endpoint.wire_slot_count == 0 ? endpoint.format.channels : endpoint.wire_slot_count;
}
}  // namespace

i2s_data_bit_width_t WireWidth(const I2sEndpointProfile& endpoint) {
    return endpoint.wire_bits_per_sample == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
}

i2s_std_config_t MakeStdConfig(const I2sEndpointProfile& endpoint, bool tx, const I2sEndpointProfile* peer) {
    const i2s_slot_mode_t mode = WireSlotCount(endpoint) == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    // 全双工（外部 Codec）时两侧 init 都填 dout+din，对齐官方 CreateDuplexChannels
    // 的同一 config 双 init 做法，避免 ESP-IDF 双工通道 GPIO 一致性校验失败。
    const int peer_data = peer != nullptr ? peer->data : I2S_GPIO_UNUSED;
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(endpoint.format.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(WireWidth(endpoint), mode),
        .gpio_cfg =
            {
                .mclk = endpoint.mclk == -1 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(endpoint.mclk),
                .bclk = static_cast<gpio_num_t>(endpoint.bclk),
                .ws = static_cast<gpio_num_t>(endpoint.ws),
                .dout = tx ? static_cast<gpio_num_t>(endpoint.data)
                           : (peer != nullptr ? static_cast<gpio_num_t>(peer_data) : I2S_GPIO_UNUSED),
                .din = tx ? (peer != nullptr ? static_cast<gpio_num_t>(peer_data) : I2S_GPIO_UNUSED)
                          : static_cast<gpio_num_t>(endpoint.data),
                .invert_flags = {},
            },
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // 板级 I2S 配置审计（MCLK 频率/slot mode/mask）。
    ESP_LOGI("sparkbot_i2s", "I2S_CFG mclk=%d mclk_hz=%llu slot_mode=%d slot_mask=%d bits=%d sample_rate=%d",
             static_cast<int>(endpoint.mclk), static_cast<unsigned long long>(endpoint.format.sample_rate_hz) * 256ULL,
             static_cast<int>(mode),
             static_cast<int>(WireSlotCount(endpoint) == 1 ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH),
             static_cast<int>(WireWidth(endpoint)), static_cast<int>(endpoint.format.sample_rate_hz));
    config.slot_cfg.slot_mask = WireSlotCount(endpoint) == 1 ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH;
    // 与官方小智/MVP 的 NoAudioCodec 一致：数据左对齐（MSB 对齐 slot 高位）。
    // 我们写 32bit wire（16bit PCM << pcm_shift_bits），若 left_align=false（右对齐）
    // 高 16 位数据会被当作低位处理，导致功放无声。
#if SOC_I2S_HW_VERSION_2
    config.slot_cfg.left_align = true;
#endif
    return config;
}

int16_t ToPcm16(int32_t raw, const I2sEndpointProfile& endpoint) {
    int64_t value = raw;
    if (endpoint.wire_bits_per_sample == 32) {
        value >>= endpoint.pcm_shift_bits;
    }
    value = std::clamp(value, static_cast<int64_t>(std::numeric_limits<int16_t>::min()),
                       static_cast<int64_t>(std::numeric_limits<int16_t>::max()));
    return static_cast<int16_t>(value);
}

int32_t ToWire(int16_t pcm, const I2sEndpointProfile& endpoint) {
    if (endpoint.wire_bits_per_sample == 16) {
        return pcm;
    }
    const int64_t shifted = static_cast<int64_t>(pcm) << endpoint.pcm_shift_bits;
    return static_cast<int32_t>(std::clamp(shifted, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                                           static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

}  // namespace detail

Status Esp32s3PcmAudioPorts::Impl::TryInitializeChannelsLocked() {
    if (!input_open_ || !output_open_ || channels_ready_) {
        return Status::Ok();
    }
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(profile_.playback_i2s.port, I2S_ROLE_MASTER);
    config.dma_desc_num = profile_.dma_desc_num;
    config.dma_frame_num = profile_.dma_frame_num;
    config.auto_clear_after_cb = true;

    esp_err_t error = ESP_OK;
    const char* failed_stage = "";
    if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex ||
        profile_.capture_i2s.port == profile_.playback_i2s.port) {
        error = i2s_new_channel(&config, &tx_channel_, &rx_channel_);
        failed_stage = "duplex";
    } else {
        error = i2s_new_channel(&config, &tx_channel_, nullptr);
        if (error == ESP_OK) {
            config.id = static_cast<int>(profile_.capture_i2s.port);
            error = i2s_new_channel(&config, nullptr, &rx_channel_);
            failed_stage = "rx";
        } else {
            failed_stage = "tx";
        }
    }
    if (error != ESP_OK) {
        DestroyChannelsLocked();
        return detail::Unavailable(std::string("创建 ESP32-S3 I2S 通道失败 stage=") + failed_stage +
                                   " error=" + esp_err_to_name(error));
    }

    I2sEndpointProfile playback_endpoint = profile_.playback_i2s;
    if (playback_format_.has_value()) {
        playback_endpoint.format = *playback_format_;
    }
    const bool full_duplex = profile_.topology == AudioBoardTopology::kExternalCodecDuplex;
    const i2s_std_config_t tx_config =
        detail::MakeStdConfig(playback_endpoint, true, full_duplex ? &profile_.capture_i2s : nullptr);
    const i2s_std_config_t rx_config =
        detail::MakeStdConfig(profile_.capture_i2s, false, full_duplex ? &playback_endpoint : nullptr);
    error = i2s_channel_init_std_mode(tx_channel_, &tx_config);
    if (error == ESP_OK) {
        error = i2s_channel_init_std_mode(rx_channel_, &rx_config);
    }
    if (error != ESP_OK) {
        DestroyChannelsLocked();
        return detail::Unavailable("初始化 ESP32-S3 I2S 标准模式失败");
    }
    channels_ready_ = true;
    return Status::Ok();
}

void Esp32s3PcmAudioPorts::Impl::DestroyChannels() {
    std::lock_guard<std::mutex> lock(mutex_);
    DestroyChannelsLocked();
}

void Esp32s3PcmAudioPorts::Impl::DestroyChannelsLocked() {
    if (tx_channel_ != nullptr) {
        if (output_running_) {
            i2s_channel_disable(tx_channel_);
        }
        i2s_del_channel(tx_channel_);
    }
    if (rx_channel_ != nullptr) {
        if (input_running_) {
            i2s_channel_disable(rx_channel_);
        }
        i2s_del_channel(rx_channel_);
    }
    tx_channel_ = nullptr;
    rx_channel_ = nullptr;
    channels_ready_ = false;
}

void Esp32s3PcmAudioPorts::Impl::EnqueueInput(voice::AudioFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnqueueInputLocked(std::move(frame));
}

void Esp32s3PcmAudioPorts::Impl::EnqueueInputLocked(voice::AudioFrame frame) {
    if (!input_running_) {
        return;
    }
    if (input_queue_.size() >= options_.input_queue_depth) {
        input_queue_.pop_front();
        ++dropped_input_frames_;
    }
    input_queue_.push_back(std::move(frame));
    input_high_watermark_.store(std::max(input_high_watermark_.load(), input_queue_.size()));
    input_cv_.notify_one();
}

void Esp32s3PcmAudioPorts::Impl::CaptureTaskEntry(void* arg) {
    auto* self = static_cast<Impl*>(arg);
    self->CaptureLoop();
    vTaskDelete(nullptr);
}

void Esp32s3PcmAudioPorts::Impl::DeliveryTaskEntry(void* arg) {
    auto* self = static_cast<Impl*>(arg);
    self->DeliveryLoop();
    vTaskDelete(nullptr);
}

void Esp32s3PcmAudioPorts::Impl::OutputTaskEntry(void* arg) {
    auto* self = static_cast<Impl*>(arg);
    self->OutputLoop();
    vTaskDeleteWithCaps(nullptr);
}

void Esp32s3PcmAudioPorts::Impl::MarkTaskDone(TaskHandle_t* task) {
    std::lock_guard<std::mutex> lock(mutex_);
    *task = nullptr;
    done_cv_.notify_all();
}

void Esp32s3PcmAudioPorts::Impl::CaptureLoop() {
    const auto& endpoint = profile_.capture_i2s;
    const std::size_t pcm_samples_per_period = static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
                                               endpoint.format.frame_duration_ms / 1000U * endpoint.format.channels;
    std::vector<int16_t> pcm(pcm_samples_per_period);
    const bool codec_owned_io = profile_.topology == AudioBoardTopology::kExternalCodecDuplex;
    const std::size_t wire_samples_per_period = static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
                                                endpoint.format.frame_duration_ms / 1000U *
                                                detail::WireSlotCount(endpoint);
    const std::size_t wire_size = wire_samples_per_period * detail::WireBytes(endpoint);
    std::vector<uint8_t> wire(codec_owned_io ? 0 : wire_size);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!input_running_) {
                break;
            }
        }
        if (codec_owned_io) {
            // esp_codec_dev owns the ES8311's post-open mono slot format. Its
            // API matches XiaoZhi: a contiguous int16 mono buffer, no manual
            // interleaving based on the pre-open physical stereo config.
            if (!ReadEs8311Pcm(codec_dev_, pcm.data(), pcm.size()).ok()) {
                ++short_reads_;
                ++input_i2s_errors_;
                continue;
            }
        } else {
            size_t bytes_read = 0;
            const esp_err_t error =
                i2s_channel_read(rx_channel_, wire.data(), wire.size(), &bytes_read, options_.io_timeout_ms);
            if (error != ESP_OK || bytes_read != wire.size()) {
                if (input_running_) {
                    ++short_reads_;
                    if (error != ESP_OK) ++input_i2s_errors_;
                }
                continue;
            }
            if (endpoint.wire_bits_per_sample == 32) {
                const auto* raw = reinterpret_cast<const int32_t*>(wire.data());
                for (std::size_t i = 0; i < pcm_samples_per_period; ++i) {
                    pcm[i] = detail::ToPcm16(raw[i * detail::WireSlotCount(endpoint)], endpoint);
                }
            } else {
                const auto* raw = reinterpret_cast<const int16_t*>(wire.data());
                for (std::size_t i = 0; i < pcm_samples_per_period; ++i) {
                    pcm[i] = raw[i * detail::WireSlotCount(endpoint)];
                }
            }
        }
        uint64_t sum_squares = 0;
        uint16_t peak = 0;
        bool all_zero = true;
        for (const int16_t sample : pcm) {
            const uint16_t absolute = AbsolutePcm16(sample);
            peak = std::max(peak, absolute);
            sum_squares += static_cast<uint64_t>(static_cast<int32_t>(sample) * static_cast<int32_t>(sample));
            all_zero = all_zero && sample == 0;
        }
        input_pcm_bytes_ += pcm.size() * sizeof(int16_t);
        input_samples_ += pcm.size();
        input_sum_squares_ += sum_squares;
        if (all_zero) ++input_zero_periods_;
        RaisePeak(input_peak_, peak);
        const Status status = test_input_enabled_.load()
                                  ? Status::Ok()
                                  : assembler_->Push(pcm.data(), pcm.size(), [this](voice::AudioFrame frame) {
                                        EnqueueInput(std::move(frame));
                                        return Status::Ok();
                                    });
        if (!status.ok()) {
            ++dropped_input_frames_;
        }
    }
    MarkTaskDone(&capture_task_);
}

void Esp32s3PcmAudioPorts::Impl::DeliveryLoop() {
    std::size_t delivered_frames = 0;
    while (true) {
        voice::AudioFrame frame;
        voice::AudioFrameSink sink;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            input_cv_.wait(lock, [this]() { return !input_queue_.empty() || !input_running_; });
            if (input_queue_.empty() && !input_running_) {
                break;
            }
            frame = std::move(input_queue_.front());
            input_queue_.pop_front();
            sink = input_sink_;
        }
        if (!sink || !sink(std::move(frame)).ok()) {
            ++dropped_input_frames_;
        } else {
            ++captured_frames_;
            ++delivered_frames;
            if (delivered_frames % 128 == 0) {
                const UBaseType_t remaining_words = uxTaskGetStackHighWaterMark(nullptr);
                ESP_LOGI(detail::kAudioRuntimeTag, "DELIVERY_TASK_STACK remaining_bytes=%u",
                         static_cast<unsigned>(remaining_words * sizeof(StackType_t)));
            }
        }
    }
    MarkTaskDone(&delivery_task_);
}

Status Esp32s3PcmAudioPorts::Impl::WriteFrame(const voice::AudioFrame& frame) {
    I2sEndpointProfile endpoint = profile_.playback_i2s;
    if (playback_format_.has_value()) {
        endpoint.format = *playback_format_;
    }
    if (frame.format.codec != voice::AudioCodec::kPcmS16Le || frame.format.bits_per_sample != 16 ||
        frame.format.channels != endpoint.format.channels || frame.format.sample_rate_hz == 0) {
        return detail::Invalid("播放帧格式不支持");
    }
    std::size_t sample_count = frame.payload.size() / (sizeof(int16_t) * endpoint.format.channels);
    const auto* pcm = reinterpret_cast<const int16_t*>(frame.payload.data());
    if (frame.format.sample_rate_hz != endpoint.format.sample_rate_hz) {
        const uint64_t destination_samples =
            static_cast<uint64_t>(sample_count) * endpoint.format.sample_rate_hz / frame.format.sample_rate_hz;
        if (destination_samples == 0 || destination_samples > codec_pcm_scratch_.capacity()) {
            return detail::Invalid("播放帧重采样超过启动期 PCM scratch 预算");
        }
        codec_pcm_scratch_.resize(static_cast<std::size_t>(destination_samples));
        for (std::size_t index = 0; index < codec_pcm_scratch_.size(); ++index) {
            const uint64_t source_position = static_cast<uint64_t>(index) * frame.format.sample_rate_hz;
            const std::size_t lower = static_cast<std::size_t>(source_position / endpoint.format.sample_rate_hz);
            const std::size_t upper = lower + 1 < sample_count ? lower + 1 : lower;
            const uint32_t fraction = static_cast<uint32_t>(source_position % endpoint.format.sample_rate_hz);
            const int64_t interpolated =
                static_cast<int64_t>(pcm[lower]) * (endpoint.format.sample_rate_hz - fraction) +
                static_cast<int64_t>(pcm[upper]) * fraction;
            codec_pcm_scratch_[index] = static_cast<int16_t>(interpolated / endpoint.format.sample_rate_hz);
        }
        pcm = codec_pcm_scratch_.data();
        sample_count = codec_pcm_scratch_.size();
        ++resampled_frames_;
    }
    if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex) {
        // The ES8311 owns the I2S format after esp_codec_dev_open(). Feed its
        // single-channel PCM API directly, matching the official SparkBot
        // codec; do not recreate the stale physical slot layout here.
        if (!WriteEs8311Pcm(codec_dev_, pcm, sample_count).ok()) {
            ++short_writes_;
            ++output_i2s_errors_;
            return detail::Unavailable("ES8311 播放 PCM 失败");
        }
        uint64_t sum_squares = 0;
        uint16_t peak = 0;
        bool all_zero = true;
        for (std::size_t index = 0; index < sample_count; ++index) {
            const int16_t sample = pcm[index];
            const uint16_t absolute = AbsolutePcm16(sample);
            peak = std::max(peak, absolute);
            sum_squares += static_cast<uint64_t>(static_cast<int32_t>(sample) * static_cast<int32_t>(sample));
            all_zero = all_zero && sample == 0;
        }
        output_pcm_bytes_ += sample_count * sizeof(int16_t);
        output_samples_ += sample_count;
        output_sum_squares_ += sum_squares;
        if (all_zero) ++output_zero_periods_;
        RaisePeak(output_peak_, peak);
        return Status::Ok();
    }
    static bool s_first_write_logged = false;
    if (!s_first_write_logged) {
        s_first_write_logged = true;
        const int vol = output_volume_.load();
        ESP_LOGI(voicelife::audio_esp::detail::kAudioRuntimeTag,
                 "I2S_WRITE first_frame bytes=%u samples=%u volume=%d sr=%u wire=%u shift=%u",
                 static_cast<unsigned>(frame.payload.size()), static_cast<unsigned>(sample_count), vol,
                 endpoint.format.sample_rate_hz, endpoint.wire_bits_per_sample, endpoint.pcm_shift_bits);
    }
    const std::size_t period_samples = static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
                                       endpoint.format.frame_duration_ms / 1000U * endpoint.format.channels;
    for (std::size_t offset = 0; offset < sample_count; offset += period_samples) {
        const std::size_t count = std::min(period_samples, sample_count - offset);
        const std::size_t wire_samples = count * detail::WireSlotCount(endpoint) / endpoint.format.channels;
        const std::size_t bytes = wire_samples * detail::WireBytes(endpoint);
        wire_scratch_.resize(bytes);
        if (endpoint.wire_bits_per_sample == 32) {
            auto* out = reinterpret_cast<int32_t*>(wire_scratch_.data());
            const int volume = output_volume_.load();
            for (std::size_t i = 0; i < count; ++i) {
                // 播放增益：MVP 用 (vol/100)^2*65536 满幅；语音信号约 -6~-12dBFS，
                // volume=100 时补 4 倍（+12dB）数字增益，clamp 防削波。
                const int32_t gain = 4;
                const int32_t scaled = static_cast<int32_t>(pcm[offset + i]) * volume * gain / 100;
                if (scaled > 32767 || scaled < -32768) ++output_clipped_samples_;
                const int16_t clamped = static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767));
                const int32_t value = detail::ToWire(clamped, endpoint);
                for (uint8_t slot = 0; slot < detail::WireSlotCount(endpoint); ++slot) {
                    out[i * detail::WireSlotCount(endpoint) + slot] = value;
                }
            }
        } else {
            const int volume = output_volume_.load();
            auto* out = reinterpret_cast<int16_t*>(wire_scratch_.data());
            for (std::size_t i = 0; i < count; ++i) {
                const int32_t scaled = static_cast<int32_t>(pcm[offset + i]) * volume / 100;
                if (scaled > 32767 || scaled < -32768) ++output_clipped_samples_;
                const int16_t value = static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767));
                for (uint8_t slot = 0; slot < detail::WireSlotCount(endpoint); ++slot) {
                    out[i * detail::WireSlotCount(endpoint) + slot] = value;
                }
            }
        }
        size_t bytes_written = 0;
        uint64_t sum_squares = 0;
        uint16_t peak = 0;
        bool all_zero = true;
        for (std::size_t i = 0; i < count; ++i) {
            const int32_t gain = endpoint.wire_bits_per_sample == 32 ? 4 : 1;
            const int32_t scaled = static_cast<int32_t>(pcm[offset + i]) * output_volume_.load() * gain / 100;
            const int16_t clamped = static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767));
            const uint16_t absolute = AbsolutePcm16(clamped);
            peak = std::max(peak, absolute);
            sum_squares += static_cast<uint64_t>(static_cast<int32_t>(clamped) * static_cast<int32_t>(clamped));
            all_zero = all_zero && clamped == 0;
        }
        const esp_err_t error = i2s_channel_write(tx_channel_, wire_scratch_.data(), wire_scratch_.size(),
                                                  &bytes_written, options_.io_timeout_ms);
        if (error != ESP_OK || bytes_written != wire_scratch_.size()) {
            ++short_writes_;
            if (error != ESP_OK) ++output_i2s_errors_;
            return detail::Unavailable("I2S 播放返回短写或超时");
        }
        output_pcm_bytes_ += count * sizeof(int16_t);
        output_samples_ += count;
        output_sum_squares_ += sum_squares;
        if (all_zero) ++output_zero_periods_;
        RaisePeak(output_peak_, peak);
    }
    return Status::Ok();
}

void Esp32s3PcmAudioPorts::Impl::OutputLoop() {
    while (true) {
        voice::AudioFrame frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            output_cv_.wait(lock, [this]() { return !output_queue_.empty() || !output_running_; });
            if (output_queue_.empty() && !output_running_) {
                break;
            }
            frame = std::move(output_queue_.front());
            output_queue_.pop_front();
            const uint64_t frame_duration_ms = detail::PcmDurationMs(frame);
            if (frame_duration_ms <= output_queue_duration_ms_) {
                output_queue_duration_ms_ -= frame_duration_ms;
            } else {
                output_queue_duration_ms_ = 0;
            }
            output_space_cv_.notify_one();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            output_writing_ = true;
        }
        if (WriteFrame(frame).ok()) {
            ++played_frames_;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            output_writing_ = false;
            if (amplifier_disable_pending_ && output_queue_.empty() && amplifier_enabled_ && amplifier_callback_) {
                amplifier_callback_(false);
                amplifier_enabled_ = false;
            }
            amplifier_disable_pending_ = false;
        }
    }
    MarkTaskDone(&output_task_);
    bool finalize_close = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finalize_close = output_closing_;
    }
    if (finalize_close) {
        (void)FinalizeOutputClose();
    }
}

}  // namespace voicelife::audio_esp

#endif  // ESP_PLATFORM
