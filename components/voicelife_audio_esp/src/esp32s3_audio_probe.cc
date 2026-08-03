#include "voicelife/audio_esp/esp32s3_audio_probe.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_system.h"

namespace voicelife::audio_esp {
namespace {

constexpr char kTag[] = "voicelife_audio_probe";

Status EspFailure(const char* operation, esp_err_t error) {
    return Status::Error(ErrorCode::kUnavailable,
                         std::string(operation) + " 失败，esp_err_t=" + std::to_string(error));
}

size_t WireBytes(const I2sEndpointProfile& endpoint) {
    return endpoint.wire_bits_per_sample / 8U;
}

i2s_data_bit_width_t WireWidth(const I2sEndpointProfile& endpoint) {
    return endpoint.wire_bits_per_sample == 32 ? I2S_DATA_BIT_WIDTH_32BIT
                                               : I2S_DATA_BIT_WIDTH_16BIT;
}

i2s_std_config_t MakeStdConfig(const I2sEndpointProfile& endpoint, bool tx) {
    const i2s_slot_mode_t mode = endpoint.format.channels == 1 ? I2S_SLOT_MODE_MONO
                                                                 : I2S_SLOT_MODE_STEREO;
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(endpoint.format.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(WireWidth(endpoint), mode),
        .gpio_cfg = {
            .mclk = endpoint.mclk == -1 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(endpoint.mclk),
            .bclk = static_cast<gpio_num_t>(endpoint.bclk),
            .ws = static_cast<gpio_num_t>(endpoint.ws),
            .dout = tx ? static_cast<gpio_num_t>(endpoint.data) : I2S_GPIO_UNUSED,
            .din = tx ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(endpoint.data),
            .invert_flags = {},
        },
    };
    if (endpoint.format.channels == 1) {
        config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    } else {
        config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    }
    return config;
}

int16_t ToPcm16(int32_t raw, const I2sEndpointProfile& endpoint) {
    int32_t value = raw;
    if (endpoint.wire_bits_per_sample == 32) {
        value >>= endpoint.pcm_shift_bits;
    }
    value = std::clamp(value, static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                       static_cast<int32_t>(std::numeric_limits<int16_t>::max()));
    return static_cast<int16_t>(value);
}

int32_t ToWire(int16_t pcm, const I2sEndpointProfile& endpoint, uint8_t attenuation_bits) {
    const int32_t attenuated = static_cast<int32_t>(pcm) >> attenuation_bits;
    if (endpoint.wire_bits_per_sample == 16) {
        return attenuated;
    }
    const int64_t shifted = static_cast<int64_t>(attenuated) << endpoint.pcm_shift_bits;
    return static_cast<int32_t>(std::clamp(
        shifted, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

void RecordPcm(AudioProbeReport& report, std::vector<int16_t>& captured, int16_t sample,
               int16_t& previous, bool& has_previous) {
    const int32_t magnitude = sample == std::numeric_limits<int16_t>::min()
                                  ? 32768
                                  : std::abs(static_cast<int32_t>(sample));
    if (sample != 0) {
        ++report.nonzero_samples;
    }
    if (has_previous && sample != previous) {
        ++report.changed_samples;
    }
    if (sample == std::numeric_limits<int16_t>::min() ||
        sample == std::numeric_limits<int16_t>::max()) {
        ++report.saturated_samples;
    }
    previous = sample;
    has_previous = true;
    report.peak_abs = std::max(report.peak_abs, static_cast<uint32_t>(magnitude));
    report.sum_squares += static_cast<uint64_t>(magnitude) * static_cast<uint64_t>(magnitude);
    ++report.capture_samples;
    captured.push_back(sample);
}

}  // namespace

class Esp32s3AudioProbe::Impl final {
   public:
    ~Impl() { Close(); }

    Result<AudioProbeReport> Run(const AudioBoardProfile& profile,
                                 const AudioProbeOptions& options) {
        const Status validation = profile.Validate();
        if (!validation.ok()) {
            return Result<AudioProbeReport>::Failure(validation.code, validation.message);
        }
        Close();

        AudioProbeReport report;
        report.codec_control_required = profile.codec_control.has_value();
        if (profile.codec_control.has_value()) {
            const auto& control = *profile.codec_control;
            i2c_master_bus_config_t bus_config = {};
            bus_config.i2c_port = static_cast<i2c_port_num_t>(control.i2c_port);
            bus_config.sda_io_num = static_cast<gpio_num_t>(control.i2c.sda);
            bus_config.scl_io_num = static_cast<gpio_num_t>(control.i2c.scl);
            bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
            bus_config.glitch_ignore_cnt = 7;
            bus_config.flags.enable_internal_pullup = 1;
            esp_err_t error = i2c_new_master_bus(&bus_config, &i2c_bus_);
            if (error != ESP_OK) {
                return Result<AudioProbeReport>::Failure(
                    ErrorCode::kUnavailable, EspFailure("创建音频 I2C 总线", error).message);
            }
            report.i2c_bus_ready = true;
            report.es8311_ack = i2c_master_probe(
                                   i2c_bus_, control.addresses.es8311_8bit >> 1,
                                   static_cast<int>(options.timeout_ms)) == ESP_OK;
            report.es7210_ack = i2c_master_probe(
                                   i2c_bus_, control.addresses.es7210_8bit >> 1,
                                   static_cast<int>(options.timeout_ms)) == ESP_OK;
            report.pca9557_ack = i2c_master_probe(
                                    i2c_bus_, control.addresses.pca9557_7bit,
                                    static_cast<int>(options.timeout_ms)) == ESP_OK;
        }

        esp_err_t error = ESP_OK;
        i2s_chan_config_t channel_config =
            I2S_CHANNEL_DEFAULT_CONFIG(profile.playback_i2s.port, I2S_ROLE_MASTER);
        channel_config.dma_desc_num = profile.dma_desc_num;
        channel_config.dma_frame_num = profile.dma_frame_num;
        channel_config.auto_clear_after_cb = true;

        if (profile.topology == AudioBoardTopology::kExternalCodecDuplex) {
            error = i2s_new_channel(&channel_config, &tx_channel_, &rx_channel_);
        } else {
            error = i2s_new_channel(&channel_config, &tx_channel_, nullptr);
            if (error == ESP_OK) {
                channel_config.id = static_cast<int>(profile.capture_i2s.port);
                error = i2s_new_channel(&channel_config, nullptr, &rx_channel_);
            }
        }
        if (error != ESP_OK) {
            Close();
            return Result<AudioProbeReport>::Failure(
                ErrorCode::kUnavailable, EspFailure("创建 I2S 通道", error).message);
        }

        const i2s_std_config_t tx_config = MakeStdConfig(profile.playback_i2s, true);
        const i2s_std_config_t rx_config = MakeStdConfig(profile.capture_i2s, false);
        error = i2s_channel_init_std_mode(tx_channel_, &tx_config);
        if (error == ESP_OK) {
            error = i2s_channel_init_std_mode(rx_channel_, &rx_config);
        }
        if (error != ESP_OK) {
            Close();
            return Result<AudioProbeReport>::Failure(
                ErrorCode::kUnavailable, EspFailure("初始化 I2S 标准模式", error).message);
        }
        report.i2s_channels_ready = true;

        error = i2s_channel_enable(tx_channel_);
        if (error == ESP_OK) {
            error = i2s_channel_enable(rx_channel_);
        }
        if (error != ESP_OK) {
            Close();
            return Result<AudioProbeReport>::Failure(
                ErrorCode::kUnavailable, EspFailure("启动 I2S 通道", error).message);
        }
        report.i2s_channels_started = true;

        const size_t playback_frames = static_cast<size_t>(
            profile.playback_i2s.format.sample_rate_hz *
            profile.playback_i2s.format.frame_duration_ms / 1000U);
        const size_t playback_samples = playback_frames * profile.playback_i2s.format.channels;
        const size_t playback_bytes = playback_samples * WireBytes(profile.playback_i2s);
        std::vector<uint8_t> silence(playback_bytes, 0);
        size_t bytes_written = 0;
        error = i2s_channel_write(tx_channel_, silence.data(), silence.size(), &bytes_written,
                                  options.timeout_ms);
        report.bytes_written = bytes_written;
        if (error != ESP_OK || bytes_written != silence.size()) {
            Close();
            return Result<AudioProbeReport>::Failure(
                ErrorCode::kUnavailable,
                error == ESP_OK ? "I2S 静音写入返回短写" : EspFailure("I2S 静音写入", error).message);
        }

        const size_t capture_frames_per_read = static_cast<size_t>(
            profile.capture_i2s.format.sample_rate_hz *
            profile.capture_i2s.format.frame_duration_ms / 1000U);
        const size_t capture_channels = profile.capture_i2s.format.channels;
        const size_t capture_wire_bytes = capture_frames_per_read * capture_channels *
                                           WireBytes(profile.capture_i2s);
        const size_t read_count = std::max<size_t>(
            1, (options.capture_duration_ms + profile.capture_i2s.format.frame_duration_ms - 1U) /
                   profile.capture_i2s.format.frame_duration_ms);
        std::vector<int16_t> captured;
        captured.reserve(read_count * capture_frames_per_read * capture_channels);
        int16_t previous = 0;
        bool has_previous = false;
        for (size_t i = 0; i < read_count; ++i) {
            size_t bytes_read = 0;
            if (profile.capture_i2s.wire_bits_per_sample == 32) {
                std::vector<int32_t> wire(capture_frames_per_read * capture_channels);
                error = i2s_channel_read(rx_channel_, wire.data(), capture_wire_bytes, &bytes_read,
                                          options.timeout_ms);
                if (error == ESP_OK && bytes_read == capture_wire_bytes) {
                    for (int32_t raw : wire) {
                        RecordPcm(report, captured,
                                  ToPcm16(raw, profile.capture_i2s), previous, has_previous);
                    }
                }
            } else {
                std::vector<int16_t> wire(capture_frames_per_read * capture_channels);
                error = i2s_channel_read(rx_channel_, wire.data(), capture_wire_bytes, &bytes_read,
                                          options.timeout_ms);
                if (error == ESP_OK && bytes_read == capture_wire_bytes) {
                    for (int16_t raw : wire) {
                        RecordPcm(report, captured, ToPcm16(raw, profile.capture_i2s), previous,
                                  has_previous);
                    }
                }
            }
            report.bytes_read += bytes_read;
            if (error != ESP_OK || bytes_read != capture_wire_bytes) {
                Close();
                return Result<AudioProbeReport>::Failure(
                    ErrorCode::kUnavailable,
                    error == ESP_OK ? "I2S 采集返回短读" : EspFailure("I2S 采集", error).message);
            }
        }

        if (options.replay_capture && !captured.empty()) {
            std::vector<uint8_t> replay(playback_bytes, 0);
            const size_t capture_frames = captured.size() / capture_channels;
            if (capture_frames > 0) {
                for (size_t frame = 0; frame < playback_frames; ++frame) {
                    const size_t source_frame = std::min<size_t>(
                        capture_frames - 1,
                        frame * profile.capture_i2s.format.sample_rate_hz /
                            profile.playback_i2s.format.sample_rate_hz);
                    const int16_t source = captured[source_frame * capture_channels];
                    const int32_t wire_value =
                        ToWire(source, profile.playback_i2s, /*attenuation_bits=*/4);
                    for (uint8_t channel = 0; channel < profile.playback_i2s.format.channels;
                         ++channel) {
                        const size_t offset =
                            (frame * profile.playback_i2s.format.channels + channel) *
                            WireBytes(profile.playback_i2s);
                        if (profile.playback_i2s.wire_bits_per_sample == 32) {
                            std::memcpy(replay.data() + offset, &wire_value, sizeof(wire_value));
                        } else {
                            const int16_t value = static_cast<int16_t>(wire_value);
                            std::memcpy(replay.data() + offset, &value, sizeof(value));
                        }
                    }
                }
            }
            size_t replay_written = 0;
            error = i2s_channel_write(tx_channel_, replay.data(), replay.size(), &replay_written,
                                      options.timeout_ms);
            report.replay_bytes_written = replay_written;
            if (error != ESP_OK || replay_written != replay.size()) {
                Close();
                return Result<AudioProbeReport>::Failure(
                    ErrorCode::kUnavailable,
                    error == ESP_OK ? "I2S 回放返回短写" : EspFailure("I2S 回放", error).message);
            }
        }

        report.minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
        ESP_LOGI(kTag,
                 "PCM evidence: samples=%u nonzero=%u changed=%u saturated=%u "
                 "saturation_ppm=%llu peak=%u mean_square=%llu bus_read=%u bus_write=%u "
                 "replay=%u min_heap=%u",
                 static_cast<unsigned>(report.capture_samples),
                 static_cast<unsigned>(report.nonzero_samples),
                 static_cast<unsigned>(report.changed_samples),
                 static_cast<unsigned>(report.saturated_samples),
                 static_cast<unsigned long long>(report.saturation_ratio_ppm()),
                 static_cast<unsigned>(report.peak_abs),
                 static_cast<unsigned long long>(report.mean_square()),
                 static_cast<unsigned>(report.bytes_read),
                 static_cast<unsigned>(report.bytes_written),
                 static_cast<unsigned>(report.replay_bytes_written),
                 static_cast<unsigned>(report.minimum_free_heap_bytes));
        Close();
        return Result<AudioProbeReport>::Success(report);
    }

   private:
    void Close() {
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
        if (i2c_bus_ != nullptr) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
        }
    }

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2s_chan_handle_t tx_channel_ = nullptr;
    i2s_chan_handle_t rx_channel_ = nullptr;
};

Esp32s3AudioProbe::Esp32s3AudioProbe() : impl_(std::make_unique<Impl>()) {}
Esp32s3AudioProbe::~Esp32s3AudioProbe() = default;

Result<AudioProbeReport> Esp32s3AudioProbe::Run(const AudioBoardProfile& profile,
                                                const AudioProbeOptions& options) {
    return impl_->Run(profile, options);
}

}  // namespace voicelife::audio_esp

#else

namespace voicelife::audio_esp {

class Esp32s3AudioProbe::Impl {};

Esp32s3AudioProbe::Esp32s3AudioProbe() : impl_(std::make_unique<Impl>()) {}
Esp32s3AudioProbe::~Esp32s3AudioProbe() = default;

Result<AudioProbeReport> Esp32s3AudioProbe::Run(const AudioBoardProfile&,
                                                const AudioProbeOptions&) {
    return Result<AudioProbeReport>::Failure(ErrorCode::kUnavailable,
                                              "ESP32-S3 Audio Probe 只能在 ESP-IDF 目标运行");
}

}  // namespace voicelife::audio_esp

#endif
