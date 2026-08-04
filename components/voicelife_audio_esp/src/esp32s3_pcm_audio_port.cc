#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voicelife/audio_esp/pcm_frame_assembler.h"

#ifdef ESP_PLATFORM

#include "driver/i2s_std.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#endif

namespace voicelife::audio_esp {
namespace {

Status Invalid(std::string message) {
    return Status::Error(ErrorCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
    return Status::Error(ErrorCode::kUnavailable, std::move(message));
}

#ifdef ESP_PLATFORM
bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right,
                bool include_frame_duration) {
    return left.codec == right.codec && left.sample_rate_hz == right.sample_rate_hz &&
           left.channels == right.channels && left.bits_per_sample == right.bits_per_sample &&
           (!include_frame_duration || left.frame_duration_ms == right.frame_duration_ms);
}
#endif

Status ValidateNegotiatedFormat(const I2sEndpointProfile& endpoint,
                                const voice::AudioFormat& negotiated) {
    if (!negotiated.valid() || negotiated.codec != voice::AudioCodec::kPcmS16Le ||
        negotiated.bits_per_sample != 16 || negotiated.channels != endpoint.format.channels ||
        negotiated.sample_rate_hz != endpoint.format.sample_rate_hz) {
        return Invalid("协商音频格式与板级 PCM Profile 不一致");
    }
    PcmFrameAssembler assembler(negotiated, endpoint.format.frame_duration_ms);
    return assembler.Validate();
}

#ifdef ESP_PLATFORM

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
    config.slot_cfg.slot_mask = endpoint.format.channels == 1 ? I2S_STD_SLOT_LEFT
                                                                : I2S_STD_SLOT_BOTH;
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
    return static_cast<int32_t>(std::clamp(
        shifted, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

#endif

}  // namespace

class Esp32s3PcmAudioPorts::Impl final {
   public:
    class InputPort final : public voice::AudioInputPort {
       public:
        explicit InputPort(Impl& owner) : owner_(owner) {}

        void SetAudioSink(voice::AudioFrameSink sink) override {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            owner_.input_sink_ = std::move(sink);
        }

        Status Open(const voice::AudioFormat& format) override {
            return owner_.OpenInput(format);
        }

        Status StartCapture(voice::VoiceMode mode) override {
            return owner_.StartCapture(mode);
        }

        Status StopCapture() override {
            return owner_.StopCapture();
        }

        void Close() override {
            (void)owner_.CloseInput();
        }

       private:
        Impl& owner_;
    };

    class OutputPort final : public voice::AudioOutputPort {
       public:
        explicit OutputPort(Impl& owner) : owner_(owner) {}

        Status Open(const voice::AudioFormat& format) override {
            return owner_.OpenOutput(format);
        }

        Status Push(const voice::AudioFrame& frame) override {
            return owner_.PushOutput(frame);
        }

        Status Flush() override {
            return owner_.FlushOutput();
        }

        void Close() override {
            (void)owner_.CloseOutput();
        }

       private:
        Impl& owner_;
    };

    Impl(AudioBoardProfile profile, AudioPortOptions options)
        : profile_(std::move(profile)),
          options_(options),
          input_port_(*this),
          output_port_(*this) {}

    ~Impl() {
        (void)CloseOutput();
        (void)CloseInput();
        DestroyChannels();
    }

    InputPort& input() { return input_port_; }
    OutputPort& output() { return output_port_; }

    AudioPortStats stats() const {
        AudioPortStats result;
        result.captured_frames = captured_frames_.load();
        result.dropped_input_frames = dropped_input_frames_.load();
        result.played_frames = played_frames_.load();
        result.rejected_output_frames = rejected_output_frames_.load();
        result.short_reads = short_reads_.load();
        result.short_writes = short_writes_.load();
        result.input_high_watermark = input_high_watermark_.load();
        result.output_high_watermark = output_high_watermark_.load();
#ifdef ESP_PLATFORM
        result.minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
#endif
        return result;
    }

   private:
    friend class InputPort;
    friend class OutputPort;

    Status OpenInput(const voice::AudioFormat& format) {
        const Status profile_status = profile_.Validate();
        if (!profile_status.ok()) {
            return profile_status;
        }
        const Status format_status = ValidateNegotiatedFormat(profile_.capture_i2s, format);
        if (!format_status.ok()) {
            return format_status;
        }
#ifndef ESP_PLATFORM
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
        std::lock_guard<std::mutex> lock(mutex_);
        if (input_open_) {
            return capture_format_.has_value() && SameFormat(*capture_format_, format, true)
                       ? Status::Ok()
                       : Status::Error(ErrorCode::kConflict, "输入端口已经以其他格式打开");
        }
        if (profile_.topology != AudioBoardTopology::kDirectI2sSimplex) {
            return Unavailable("Codec Audio Port 尚未实现寄存器控制面");
        }
        if (options_.input_queue_depth == 0 || options_.output_queue_depth == 0) {
            return Invalid("Audio Port 队列容量不能为零");
        }
        capture_format_ = format;
        assembler_ = std::make_unique<PcmFrameAssembler>(format,
                                                          profile_.capture_i2s.format.frame_duration_ms);
        input_open_ = true;
        return TryInitializeChannelsLocked();
#endif
    }

    Status OpenOutput(const voice::AudioFormat& format) {
        const Status profile_status = profile_.Validate();
        if (!profile_status.ok()) {
            return profile_status;
        }
        const Status format_status = ValidateNegotiatedFormat(profile_.playback_i2s, format);
        if (!format_status.ok()) {
            return format_status;
        }
#ifndef ESP_PLATFORM
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_open_) {
            return playback_format_.has_value() && SameFormat(*playback_format_, format, true)
                       ? Status::Ok()
                       : Status::Error(ErrorCode::kConflict, "输出端口已经以其他格式打开");
        }
        if (profile_.topology != AudioBoardTopology::kDirectI2sSimplex) {
            return Unavailable("Codec Audio Port 尚未实现寄存器控制面");
        }
        if (options_.input_queue_depth == 0 || options_.output_queue_depth == 0) {
            return Invalid("Audio Port 队列容量不能为零");
        }
        playback_format_ = format;
        output_open_ = true;
        const Status init_status = TryInitializeChannelsLocked();
        if (!init_status.ok()) {
            output_open_ = false;
            playback_format_.reset();
            return init_status;
        }
        output_running_ = true;
        if (i2s_channel_enable(tx_channel_) != ESP_OK) {
            output_running_ = false;
            output_open_ = false;
            playback_format_.reset();
            return Unavailable("启动 I2S 播放通道失败");
        }
        if (xTaskCreate(&OutputTaskEntry, "voice_audio_out", 4096, this, 4, &output_task_) != pdPASS) {
            i2s_channel_disable(tx_channel_);
            output_running_ = false;
            output_open_ = false;
            playback_format_.reset();
            return Unavailable("创建 I2S 播放任务失败");
        }
        return Status::Ok();
#endif
    }

    Status StartCapture(voice::VoiceMode) {
#ifndef ESP_PLATFORM
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
        std::unique_lock<std::mutex> lock(mutex_);
        if (!input_open_ || !output_open_ || !channels_ready_ || !assembler_) {
            return Unavailable("输入端口尚未完成双向音频初始化");
        }
        if (input_running_) {
            return Status::Ok();
        }
        input_running_ = true;
        if (i2s_channel_enable(rx_channel_) != ESP_OK) {
            input_running_ = false;
            return Unavailable("启动 I2S 采集通道失败");
        }
        if (xTaskCreate(&CaptureTaskEntry, "voice_audio_in", 4096, this, 5, &capture_task_) != pdPASS) {
            input_running_ = false;
            i2s_channel_disable(rx_channel_);
            input_cv_.notify_all();
            return Unavailable("创建 I2S 采集任务失败");
        }
        if (xTaskCreate(&DeliveryTaskEntry, "voice_audio_sink", 3072, this, 4, &delivery_task_) != pdPASS) {
            input_running_ = false;
            i2s_channel_disable(rx_channel_);
            input_cv_.notify_all();
            const bool capture_stopped = done_cv_.wait_for(
                lock, std::chrono::milliseconds(500), [this]() { return capture_task_ == nullptr; });
            if (!capture_stopped) {
                return Unavailable("等待 I2S 采集任务退出超时");
            }
            return Unavailable("创建 I2S 音频投递任务失败");
        }
        return Status::Ok();
#endif
    }

    Status StopCapture() {
#ifndef ESP_PLATFORM
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!input_running_) {
                input_queue_.clear();
                return Status::Ok();
            }
            input_running_ = false;
            input_queue_.clear();
            if (rx_channel_ != nullptr) {
                i2s_channel_disable(rx_channel_);
            }
            input_cv_.notify_all();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        const bool stopped = done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return capture_task_ == nullptr && delivery_task_ == nullptr;
        });
        if (!stopped) {
            return Unavailable("等待 I2S 采集任务退出超时");
        }
        if (assembler_) {
            assembler_->Reset();
        }
        return Status::Ok();
#endif
    }

    Status CloseInput() {
        const Status stop_status = StopCapture();
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mutex_);
        input_sink_ = {};
        input_open_ = false;
        capture_format_.reset();
        assembler_.reset();
        if (!output_open_) {
            DestroyChannelsLocked();
        }
#else
        input_sink_ = {};
        input_open_ = false;
        capture_format_.reset();
        assembler_.reset();
#endif
        return stop_status.ok() ? Status::Ok() : stop_status;
    }

    Status PushOutput(const voice::AudioFrame& frame) {
#ifndef ESP_PLATFORM
        (void)frame;
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
        std::lock_guard<std::mutex> lock(mutex_);
        if (!output_open_ || !playback_format_.has_value()) {
            return Unavailable("输出端口尚未打开");
        }
        if (!SameFormat(frame.format, *playback_format_, true) || frame.payload.empty() ||
            frame.payload.size() % (sizeof(int16_t) * playback_format_->channels) != 0) {
            return Invalid("播放帧格式或 PCM 负载无效");
        }
        if (output_queue_.size() >= options_.output_queue_depth) {
            ++rejected_output_frames_;
            return Status::Error(ErrorCode::kConflict, "播放队列已满，拒绝新帧");
        }
        output_queue_.push_back(frame);
        output_high_watermark_.store(
            std::max(output_high_watermark_.load(), output_queue_.size()));
        output_cv_.notify_one();
        return Status::Ok();
#endif
    }

    Status FlushOutput() {
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mutex_);
        output_queue_.clear();
        return Status::Ok();
#else
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#endif
    }

    Status CloseOutput() {
#ifdef ESP_PLATFORM
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (output_open_) {
                output_running_ = false;
                output_queue_.clear();
                output_cv_.notify_all();
                if (tx_channel_ != nullptr) {
                    i2s_channel_disable(tx_channel_);
                }
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        const bool stopped = done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return output_task_ == nullptr;
        });
        output_open_ = false;
        playback_format_.reset();
        if (!input_open_) {
            DestroyChannelsLocked();
        }
        return stopped ? Status::Ok()
                       : Unavailable("等待 I2S 播放任务退出超时");
#else
        output_open_ = false;
        playback_format_.reset();
        output_queue_.clear();
        return Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#endif
    }

#ifdef ESP_PLATFORM
    Status TryInitializeChannelsLocked() {
        if (!input_open_ || !output_open_ || channels_ready_) {
            return Status::Ok();
        }
        i2s_chan_config_t config =
            I2S_CHANNEL_DEFAULT_CONFIG(profile_.playback_i2s.port, I2S_ROLE_MASTER);
        config.dma_desc_num = profile_.dma_desc_num;
        config.dma_frame_num = profile_.dma_frame_num;
        config.auto_clear_after_cb = true;

        esp_err_t error = ESP_OK;
        if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex ||
            profile_.capture_i2s.port == profile_.playback_i2s.port) {
            error = i2s_new_channel(&config, &tx_channel_, &rx_channel_);
        } else {
            error = i2s_new_channel(&config, &tx_channel_, nullptr);
            if (error == ESP_OK) {
                config.id = static_cast<int>(profile_.capture_i2s.port);
                error = i2s_new_channel(&config, nullptr, &rx_channel_);
            }
        }
        if (error != ESP_OK) {
            DestroyChannelsLocked();
            return Unavailable("创建 ESP32-S3 I2S 通道失败");
        }

        const i2s_std_config_t tx_config = MakeStdConfig(profile_.playback_i2s, true);
        const i2s_std_config_t rx_config = MakeStdConfig(profile_.capture_i2s, false);
        error = i2s_channel_init_std_mode(tx_channel_, &tx_config);
        if (error == ESP_OK) {
            error = i2s_channel_init_std_mode(rx_channel_, &rx_config);
        }
        if (error != ESP_OK) {
            DestroyChannelsLocked();
            return Unavailable("初始化 ESP32-S3 I2S 标准模式失败");
        }
        channels_ready_ = true;
        return Status::Ok();
    }

    void DestroyChannels() {
        std::lock_guard<std::mutex> lock(mutex_);
        DestroyChannelsLocked();
    }

    void DestroyChannelsLocked() {
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

    void EnqueueInput(voice::AudioFrame frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!input_running_) {
            return;
        }
        if (input_queue_.size() >= options_.input_queue_depth) {
            input_queue_.pop_front();
            ++dropped_input_frames_;
        }
        input_queue_.push_back(std::move(frame));
        input_high_watermark_.store(
            std::max(input_high_watermark_.load(), input_queue_.size()));
        input_cv_.notify_one();
    }

    static void CaptureTaskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        self->CaptureLoop();
        vTaskDelete(nullptr);
    }

    static void DeliveryTaskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        self->DeliveryLoop();
        vTaskDelete(nullptr);
    }

    static void OutputTaskEntry(void* arg) {
        auto* self = static_cast<Impl*>(arg);
        self->OutputLoop();
        vTaskDelete(nullptr);
    }

    void MarkTaskDone(TaskHandle_t* task) {
        std::lock_guard<std::mutex> lock(mutex_);
        *task = nullptr;
        done_cv_.notify_all();
    }

    void CaptureLoop() {
        const auto& endpoint = profile_.capture_i2s;
        const std::size_t samples_per_period =
            static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
            endpoint.format.frame_duration_ms / 1000U * endpoint.format.channels;
        const std::size_t wire_size = samples_per_period * WireBytes(endpoint);
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
            const esp_err_t error = i2s_channel_read(rx_channel_, wire.data(), wire.size(),
                                                     &bytes_read, options_.io_timeout_ms);
            if (error != ESP_OK || bytes_read != wire.size()) {
                if (input_running_) {
                    ++short_reads_;
                }
                continue;
            }
            if (endpoint.wire_bits_per_sample == 32) {
                const auto* raw = reinterpret_cast<const int32_t*>(wire.data());
                for (std::size_t i = 0; i < samples_per_period; ++i) {
                    pcm[i] = ToPcm16(raw[i], endpoint);
                }
            } else {
                std::memcpy(pcm.data(), wire.data(), wire.size());
            }
            const Status status = assembler_->Push(
                pcm.data(), pcm.size(), [this](voice::AudioFrame frame) {
                    EnqueueInput(std::move(frame));
                    return Status::Ok();
                });
            if (!status.ok()) {
                ++dropped_input_frames_;
            }
        }
        MarkTaskDone(&capture_task_);
    }

    void DeliveryLoop() {
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

    Status WriteFrame(const voice::AudioFrame& frame) {
        const auto& endpoint = profile_.playback_i2s;
        const std::size_t sample_count =
            frame.payload.size() / (sizeof(int16_t) * endpoint.format.channels);
        const auto* pcm = reinterpret_cast<const int16_t*>(frame.payload.data());
        const std::size_t period_samples =
            static_cast<std::size_t>(endpoint.format.sample_rate_hz) *
            endpoint.format.frame_duration_ms / 1000U * endpoint.format.channels;
        for (std::size_t offset = 0; offset < sample_count; offset += period_samples) {
            const std::size_t count = std::min(period_samples, sample_count - offset);
            const std::size_t bytes = count * WireBytes(endpoint);
            std::vector<uint8_t> wire(bytes);
            if (endpoint.wire_bits_per_sample == 32) {
                auto* out = reinterpret_cast<int32_t*>(wire.data());
                for (std::size_t i = 0; i < count; ++i) {
                    out[i] = ToWire(pcm[offset + i], endpoint);
                }
            } else {
                std::memcpy(wire.data(), pcm + offset, bytes);
            }
            size_t bytes_written = 0;
            const esp_err_t error = i2s_channel_write(tx_channel_, wire.data(), wire.size(),
                                                      &bytes_written, options_.io_timeout_ms);
            if (error != ESP_OK || bytes_written != wire.size()) {
                ++short_writes_;
                return Unavailable("I2S 播放返回短写或超时");
            }
        }
        return Status::Ok();
    }

    void OutputLoop() {
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
            if (WriteFrame(frame).ok()) {
                ++played_frames_;
            }
        }
        MarkTaskDone(&output_task_);
    }
#endif

    AudioBoardProfile profile_;
    AudioPortOptions options_;
    InputPort input_port_;
    OutputPort output_port_;
    mutable std::mutex mutex_;
    std::condition_variable input_cv_;
    std::condition_variable output_cv_;
    std::condition_variable done_cv_;
    std::deque<voice::AudioFrame> input_queue_;
    std::deque<voice::AudioFrame> output_queue_;
    voice::AudioFrameSink input_sink_;
    std::optional<voice::AudioFormat> capture_format_;
    std::optional<voice::AudioFormat> playback_format_;
    std::unique_ptr<PcmFrameAssembler> assembler_;
    bool input_open_ = false;
    bool output_open_ = false;
#ifdef ESP_PLATFORM
    bool channels_ready_ = false;
    bool input_running_ = false;
    bool output_running_ = false;
#endif

    std::atomic<std::size_t> captured_frames_{0};
    std::atomic<std::size_t> dropped_input_frames_{0};
    std::atomic<std::size_t> played_frames_{0};
    std::atomic<std::size_t> rejected_output_frames_{0};
    std::atomic<std::size_t> short_reads_{0};
    std::atomic<std::size_t> short_writes_{0};
    std::atomic<std::size_t> input_high_watermark_{0};
    std::atomic<std::size_t> output_high_watermark_{0};

#ifdef ESP_PLATFORM
    i2s_chan_handle_t tx_channel_ = nullptr;
    i2s_chan_handle_t rx_channel_ = nullptr;
    TaskHandle_t capture_task_ = nullptr;
    TaskHandle_t delivery_task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;
#else
    void DestroyChannels() {}
#endif
};

Esp32s3PcmAudioPorts::Esp32s3PcmAudioPorts(AudioBoardProfile profile, AudioPortOptions options)
    : impl_(std::make_unique<Impl>(std::move(profile), options)) {}

Esp32s3PcmAudioPorts::~Esp32s3PcmAudioPorts() = default;

voice::AudioInputPort& Esp32s3PcmAudioPorts::input() {
    return impl_->input();
}

voice::AudioOutputPort& Esp32s3PcmAudioPorts::output() {
    return impl_->output();
}

AudioPortStats Esp32s3PcmAudioPorts::stats() const {
    return impl_->stats();
}

}  // namespace voicelife::audio_esp
