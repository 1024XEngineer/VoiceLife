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

namespace detail {

bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right, bool include_frame_duration) {
    return left.codec == right.codec && left.sample_rate_hz == right.sample_rate_hz &&
           left.channels == right.channels && left.bits_per_sample == right.bits_per_sample &&
           (!include_frame_duration || left.frame_duration_ms == right.frame_duration_ms);
}

size_t WireBytes(const I2sEndpointProfile& endpoint) { return endpoint.wire_bits_per_sample / 8U; }

i2s_data_bit_width_t WireWidth(const I2sEndpointProfile& endpoint) {
    return endpoint.wire_bits_per_sample == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
}

i2s_std_config_t MakeStdConfig(const I2sEndpointProfile& endpoint, bool tx, const I2sEndpointProfile* peer) {
    const i2s_slot_mode_t mode = endpoint.format.channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
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
    config.slot_cfg.slot_mask = endpoint.format.channels == 1 ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH;
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
        i2s_channel_disable(tx_channel_);
        i2s_del_channel(tx_channel_);
    }
    if (rx_channel_ != nullptr) {
        i2s_channel_disable(rx_channel_);
        i2s_del_channel(rx_channel_);
    }
    tx_channel_ = nullptr;
    rx_channel_ = nullptr;
    channels_ready_ = false;
}

void Esp32s3PcmAudioPorts::Impl::EnqueueInput(voice::AudioFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    vTaskDelete(nullptr);
}

void Esp32s3PcmAudioPorts::Impl::MarkTaskDone(TaskHandle_t* task) {
    std::lock_guard<std::mutex> lock(mutex_);
    *task = nullptr;
    done_cv_.notify_all();
}

void Esp32s3PcmAudioPorts::Impl::CaptureLoop() {
    const auto& endpoint = profile_.capture_i2s;
    const std::size_t samples_per_period = static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
                                           endpoint.format.frame_duration_ms / 1000U * endpoint.format.channels;
    const std::size_t wire_size = samples_per_period * detail::WireBytes(endpoint);
    std::vector<uint8_t> wire(wire_size);
    std::vector<int16_t> pcm(samples_per_period);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!input_running_) {
                break;
            }
        }
        size_t bytes_read = 0;
        const esp_err_t error =
            i2s_channel_read(rx_channel_, wire.data(), wire.size(), &bytes_read, options_.io_timeout_ms);
        if (error != ESP_OK || bytes_read != wire.size()) {
            if (input_running_) {
                ++short_reads_;
            }
            continue;
        }
        if (endpoint.wire_bits_per_sample == 32) {
            const auto* raw = reinterpret_cast<const int32_t*>(wire.data());
            for (std::size_t i = 0; i < samples_per_period; ++i) {
                pcm[i] = detail::ToPcm16(raw[i], endpoint);
            }
        } else {
            std::memcpy(pcm.data(), wire.data(), wire.size());
        }
        const Status status = assembler_->Push(pcm.data(), pcm.size(), [this](voice::AudioFrame frame) {
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
        }
    }
    MarkTaskDone(&delivery_task_);
}

Status Esp32s3PcmAudioPorts::Impl::WriteFrame(const voice::AudioFrame& frame) {
    I2sEndpointProfile endpoint = profile_.playback_i2s;
    if (playback_format_.has_value()) {
        endpoint.format = *playback_format_;
    }
    const std::size_t sample_count = frame.payload.size() / (sizeof(int16_t) * endpoint.format.channels);
    const auto* pcm = reinterpret_cast<const int16_t*>(frame.payload.data());
    static bool s_first_write_logged = false;
    if (!s_first_write_logged) {
        s_first_write_logged = true;
        const int vol = output_volume_.load();
        ESP_LOGI(voicelife::audio_esp::detail::kAudioRuntimeTag,
                 "I2S_WRITE first_frame bytes=%u samples=%u volume=%d sr=%u wire=%u shift=%u first_pcm=%d",
                 static_cast<unsigned>(frame.payload.size()), static_cast<unsigned>(sample_count), vol,
                 endpoint.format.sample_rate_hz, endpoint.wire_bits_per_sample, endpoint.pcm_shift_bits,
                 sample_count > 0 ? pcm[0] : 0);
    }
    const std::size_t period_samples = static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
                                       endpoint.format.frame_duration_ms / 1000U * endpoint.format.channels;
    for (std::size_t offset = 0; offset < sample_count; offset += period_samples) {
        const std::size_t count = std::min(period_samples, sample_count - offset);
        const std::size_t bytes = count * detail::WireBytes(endpoint);
        std::vector<uint8_t> wire(bytes);
        if (endpoint.wire_bits_per_sample == 32) {
            auto* out = reinterpret_cast<int32_t*>(wire.data());
            const int volume = output_volume_.load();
            for (std::size_t i = 0; i < count; ++i) {
                // 播放增益：MVP 用 (vol/100)^2*65536 满幅；语音信号约 -6~-12dBFS，
                // volume=100 时补 4 倍（+12dB）数字增益，clamp 防削波。
                const int32_t gain = 4;
                const int32_t scaled = static_cast<int32_t>(pcm[offset + i]) * volume * gain / 100;
                out[i] = detail::ToWire(static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767)), endpoint);
            }
        } else {
            const int volume = output_volume_.load();
            if (volume == 100) {
                std::memcpy(wire.data(), pcm + offset, bytes);
            } else {
                auto* out = reinterpret_cast<int16_t*>(wire.data());
                for (std::size_t i = 0; i < count; ++i) {
                    const int32_t scaled = static_cast<int32_t>(pcm[offset + i]) * volume / 100;
                    out[i] = static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767));
                }
            }
        }
        size_t bytes_written = 0;
        const esp_err_t error =
            i2s_channel_write(tx_channel_, wire.data(), wire.size(), &bytes_written, options_.io_timeout_ms);
        if (error != ESP_OK || bytes_written != wire.size()) {
            ++short_writes_;
            return detail::Unavailable("I2S 播放返回短写或超时");
        }
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
        }
    }
    MarkTaskDone(&output_task_);
}

}  // namespace voicelife::audio_esp

#endif  // ESP_PLATFORM
