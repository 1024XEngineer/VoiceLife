#include "voicelife/audio_esp/esp32s3_audio_probe.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

namespace voicelife::audio_esp {
namespace {

constexpr char kTag[] = "voicelife_audio_probe";

Status EspFailure(const char* operation, esp_err_t error) {
    return Status::Error(ErrorCode::kUnavailable,
                         std::string(operation) + " 失败，esp_err_t=" + std::to_string(error));
}

}  // namespace

class Esp32s3AudioProbe::Impl final {
   public:
    ~Impl() { Close(); }

    Result<AudioProbeReport> Run(const AudioBoardProfile& profile, uint32_t timeout_ms) {
        const Status validation = profile.Validate();
        if (!validation.ok()) {
            return Result<AudioProbeReport>::Failure(validation.code, validation.message);
        }
        Close();
        AudioProbeReport report;
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = static_cast<i2c_port_num_t>(1);
        bus_config.sda_io_num = static_cast<gpio_num_t>(profile.i2c.sda);
        bus_config.scl_io_num = static_cast<gpio_num_t>(profile.i2c.scl);
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = 1;
        esp_err_t error = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (error != ESP_OK) {
            return Result<AudioProbeReport>::Failure(ErrorCode::kUnavailable,
                                                     EspFailure("创建音频 I2C 总线", error).message);
        }
        report.i2c_bus_ready = true;
        report.es8311_ack = i2c_master_probe(i2c_bus_, profile.codec_addresses.es8311_8bit >> 1,
                                             static_cast<int>(timeout_ms)) == ESP_OK;
        report.es7210_ack = i2c_master_probe(i2c_bus_, profile.codec_addresses.es7210_8bit >> 1,
                                             static_cast<int>(timeout_ms)) == ESP_OK;
        report.pca9557_ack = i2c_master_probe(i2c_bus_, profile.codec_addresses.pca9557_7bit,
                                              static_cast<int>(timeout_ms)) == ESP_OK;

        i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(profile.i2s_port, I2S_ROLE_MASTER);
        channel_config.dma_desc_num = profile.dma_desc_num;
        channel_config.dma_frame_num = profile.dma_frame_num;
        channel_config.auto_clear_after_cb = true;
        error = i2s_new_channel(&channel_config, &tx_channel_, &rx_channel_);
        if (error != ESP_OK) {
            Close();
            return Result<AudioProbeReport>::Failure(ErrorCode::kUnavailable,
                                                     EspFailure("创建 I2S 通道", error).message);
        }

        const uint32_t sample_rate = profile.device_playback_format.sample_rate_hz;
        i2s_std_config_t tx_config = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                              I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = static_cast<gpio_num_t>(profile.i2s.mclk),
                .bclk = static_cast<gpio_num_t>(profile.i2s.bclk),
                .ws = static_cast<gpio_num_t>(profile.i2s.ws),
                .dout = static_cast<gpio_num_t>(profile.i2s.dout),
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {},
            },
        };
        i2s_std_config_t rx_config = tx_config;
        rx_config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
        rx_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
        rx_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
        rx_config.gpio_cfg.din = static_cast<gpio_num_t>(profile.i2s.din);
        error = i2s_channel_init_std_mode(tx_channel_, &tx_config);
        if (error == ESP_OK) {
            error = i2s_channel_init_std_mode(rx_channel_, &rx_config);
        }
        if (error != ESP_OK) {
            Close();
            return Result<AudioProbeReport>::Failure(ErrorCode::kUnavailable,
                                                     EspFailure("初始化 I2S 标准模式", error).message);
        }
        report.i2s_channels_ready = true;
        error = i2s_channel_enable(tx_channel_);
        if (error == ESP_OK) {
            error = i2s_channel_enable(rx_channel_);
        }
        if (error != ESP_OK) {
            Close();
            return Result<AudioProbeReport>::Failure(ErrorCode::kUnavailable,
                                                     EspFailure("启动 I2S 通道", error).message);
        }
        report.i2s_channels_started = true;

        std::array<int16_t, 240> silence{};
        size_t bytes_written = 0;
        size_t bytes_read = 0;
        error = i2s_channel_write(tx_channel_, silence.data(), silence.size() * sizeof(int16_t),
                                   &bytes_written, timeout_ms);
        if (error == ESP_OK) {
            error = i2s_channel_read(rx_channel_, silence.data(), silence.size() * sizeof(int16_t),
                                     &bytes_read, timeout_ms);
        }
        report.bytes_written = bytes_written;
        report.bytes_read = bytes_read;
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            ESP_LOGW(kTag, "I2S 数据面 smoke 失败，esp_err_t=%d", error);
        }
        return Result<AudioProbeReport>::Success(report);
    }

   private:
    void Close() {
        if (tx_channel_ != nullptr) {
            i2s_channel_disable(tx_channel_);
        }
        if (rx_channel_ != nullptr) {
            i2s_channel_disable(rx_channel_);
        }
        if (tx_channel_ != nullptr) {
            i2s_del_channel(tx_channel_);
        }
        if (rx_channel_ != nullptr) {
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
                                                uint32_t timeout_ms) {
    return impl_->Run(profile, timeout_ms);
}

}  // namespace voicelife::audio_esp

#else

namespace voicelife::audio_esp {

class Esp32s3AudioProbe::Impl {};

Esp32s3AudioProbe::Esp32s3AudioProbe() : impl_(std::make_unique<Impl>()) {}
Esp32s3AudioProbe::~Esp32s3AudioProbe() = default;

Result<AudioProbeReport> Esp32s3AudioProbe::Run(const AudioBoardProfile&, uint32_t) {
    return Result<AudioProbeReport>::Failure(ErrorCode::kUnavailable,
                                              "ESP32-S3 Audio Probe 只能在 ESP-IDF 目标运行");
}

}  // namespace voicelife::audio_esp

#endif
