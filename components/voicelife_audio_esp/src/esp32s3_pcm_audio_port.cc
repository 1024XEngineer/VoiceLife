#include <memory>
#include <string>
#include <utility>

#include "es8311_codec_control.h"
#include "esp32s3_pcm_audio_port_internal.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace voicelife::audio_esp {

namespace detail {

Status Invalid(std::string message) { return Status::Error(ErrorCode::kInvalidArgument, std::move(message)); }

Status Unavailable(std::string message) { return Status::Error(ErrorCode::kUnavailable, std::move(message)); }

Status ValidateNegotiatedFormat(const I2sEndpointProfile& endpoint, const voice::AudioFormat& negotiated) {
    if (!negotiated.valid() || negotiated.codec != voice::AudioCodec::kPcmS16Le || negotiated.bits_per_sample != 16 ||
        negotiated.channels != endpoint.format.channels ||
        negotiated.sample_rate_hz != endpoint.format.sample_rate_hz) {
        return Invalid("协商音频格式与板级 PCM Profile 不一致");
    }
    PcmFrameAssembler assembler(negotiated, endpoint.format.frame_duration_ms);
    return assembler.Validate();
}

Status ValidatePlaybackFormat(const I2sEndpointProfile& endpoint, const voice::AudioFormat& negotiated) {
    if (!negotiated.valid() || negotiated.codec != voice::AudioCodec::kPcmS16Le || negotiated.bits_per_sample != 16 ||
        negotiated.channels != endpoint.format.channels || negotiated.sample_rate_hz < 8000 ||
        negotiated.sample_rate_hz > 48000) {
        return Invalid("协商下行 PCM 格式不受当前板级 I2S 输出支持");
    }
    return Status::Ok();
}

}  // namespace detail

void Esp32s3PcmAudioPorts::Impl::InputPort::SetAudioSink(voice::AudioFrameSink sink) {
    std::lock_guard<std::mutex> lock(owner_.mutex_);
    owner_.input_sink_ = std::move(sink);
}

Status Esp32s3PcmAudioPorts::Impl::InputPort::Open(const voice::AudioFormat& format) {
    return owner_.OpenInput(format);
}

Status Esp32s3PcmAudioPorts::Impl::InputPort::StartCapture(voice::VoiceMode mode) { return owner_.StartCapture(mode); }

Status Esp32s3PcmAudioPorts::Impl::InputPort::StopCapture() { return owner_.StopCapture(); }

void Esp32s3PcmAudioPorts::Impl::InputPort::Close() { (void)owner_.CloseInput(); }

Status Esp32s3PcmAudioPorts::Impl::OutputPort::Open(const voice::AudioFormat& format) {
    return owner_.OpenOutput(format);
}

Status Esp32s3PcmAudioPorts::Impl::OutputPort::Push(const voice::AudioFrame& frame) { return owner_.PushOutput(frame); }

Status Esp32s3PcmAudioPorts::Impl::OutputPort::Flush() { return owner_.FlushOutput(); }

bool Esp32s3PcmAudioPorts::Impl::OutputPort::IsIdle() const { return owner_.OutputIdle(); }

void Esp32s3PcmAudioPorts::Impl::OutputPort::Close() { (void)owner_.CloseOutput(); }

Esp32s3PcmAudioPorts::Impl::~Impl() {
    (void)CloseOutput();
    (void)CloseInput();
    DestroyChannels();
}

AudioPortStats Esp32s3PcmAudioPorts::Impl::stats() const {
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

Status Esp32s3PcmAudioPorts::Impl::OpenInput(const voice::AudioFormat& format) {
    const Status profile_status = profile_.Validate();
    if (!profile_status.ok()) {
        return profile_status;
    }
    const Status format_status = detail::ValidateNegotiatedFormat(profile_.capture_i2s, format);
    if (!format_status.ok()) {
        return format_status;
    }
#ifndef ESP_PLATFORM
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (input_open_) {
        return capture_format_.has_value() && detail::SameFormat(*capture_format_, format, true)
                   ? Status::Ok()
                   : Status::Error(ErrorCode::kConflict, "输入端口已经以其他格式打开");
    }
    if (profile_.topology != AudioBoardTopology::kDirectI2sSimplex &&
        profile_.topology != AudioBoardTopology::kExternalCodecDuplex) {
        return detail::Unavailable("未知音频拓扑");
    }
    if (options_.input_queue_depth == 0 || options_.output_queue_depth == 0) {
        return detail::Invalid("Audio Port 队列容量不能为零");
    }
    capture_format_ = format;
    assembler_ = std::make_unique<PcmFrameAssembler>(format, profile_.capture_i2s.format.frame_duration_ms);
    input_open_ = true;
    return TryInitializeChannelsLocked();
#endif
}

Status Esp32s3PcmAudioPorts::Impl::OpenOutput(const voice::AudioFormat& format) {
    const Status profile_status = profile_.Validate();
    if (!profile_status.ok()) {
        return profile_status;
    }
    const Status format_status = detail::ValidatePlaybackFormat(profile_.playback_i2s, format);
    if (!format_status.ok()) {
        return format_status;
    }
#ifndef ESP_PLATFORM
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (output_open_) {
        return playback_format_.has_value() && detail::SameFormat(*playback_format_, format, true)
                   ? Status::Ok()
                   : Status::Error(ErrorCode::kConflict, "输出端口已经以其他格式打开");
    }
    if (profile_.topology != AudioBoardTopology::kDirectI2sSimplex &&
        profile_.topology != AudioBoardTopology::kExternalCodecDuplex) {
        return detail::Unavailable("未知音频拓扑");
    }
    if (options_.input_queue_depth == 0 || options_.output_queue_depth == 0) {
        return detail::Invalid("Audio Port 队列容量不能为零");
    }
    // 播放端固定用板级 profile 格式（24kHz），协商/上游帧在 Push 时统一重采样。
    // 这样 I2S 时钟恒定（与 MVP 一致），避免 16k 协商导致硬件无声。
    playback_format_ = profile_.playback_i2s.format;
    output_open_ = true;
#ifdef ESP_PLATFORM
    ESP_LOGI(voicelife::audio_esp::detail::kAudioRuntimeTag, "OUTPUT_OPEN sr=%u ch=%u bits=%u",
             playback_format_->sample_rate_hz, playback_format_->channels, playback_format_->bits_per_sample);
#endif
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
        return detail::Unavailable("启动 I2S 播放通道失败");
    }
    if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex && !codec_initialized_) {
        const auto& control = *profile_.codec_control;
        const Status codec_status =
            InitializeEs8311(control.i2c_port, control.i2c.sda, control.i2c.scl, control.addresses.es8311_8bit);
        if (!codec_status.ok()) {
            i2s_channel_disable(tx_channel_);
            output_running_ = false;
            output_open_ = false;
            playback_format_.reset();
            return codec_status;
        }
        codec_initialized_ = true;
    }
    if (amplifier_callback_) {
        amplifier_callback_(true);  // 播放打开：经板级仲裁请求功放。
    }
    if (xTaskCreate(&OutputTaskEntry, "voice_audio_out", 4096, this, 4, &output_task_) != pdPASS) {
        i2s_channel_disable(tx_channel_);
        output_running_ = false;
        output_open_ = false;
        playback_format_.reset();
        return detail::Unavailable("创建 I2S 播放任务失败");
    }
    return Status::Ok();
#endif
}

Status Esp32s3PcmAudioPorts::Impl::StartCapture(voice::VoiceMode) {
#ifndef ESP_PLATFORM
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
    std::unique_lock<std::mutex> lock(mutex_);
    if (!input_open_ || !output_open_ || !channels_ready_ || !assembler_) {
        return detail::Unavailable("输入端口尚未完成双向音频初始化");
    }
    if (input_running_) {
        return Status::Ok();
    }
    input_running_ = true;
    if (i2s_channel_enable(rx_channel_) != ESP_OK) {
        input_running_ = false;
        return detail::Unavailable("启动 I2S 采集通道失败");
    }
    if (xTaskCreate(&CaptureTaskEntry, "voice_audio_in", 4096, this, 5, &capture_task_) != pdPASS) {
        input_running_ = false;
        i2s_channel_disable(rx_channel_);
        input_cv_.notify_all();
        return detail::Unavailable("创建 I2S 采集任务失败");
    }
    if (xTaskCreate(&DeliveryTaskEntry, "voice_audio_sink", 16384, this, 4, &delivery_task_) != pdPASS) {
        input_running_ = false;
        i2s_channel_disable(rx_channel_);
        input_cv_.notify_all();
        const bool capture_stopped =
            done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() { return capture_task_ == nullptr; });
        if (!capture_stopped) {
            return detail::Unavailable("等待 I2S 采集任务退出超时");
        }
        return detail::Unavailable("创建 I2S 音频投递任务失败");
    }
    return Status::Ok();
#endif
}

Status Esp32s3PcmAudioPorts::Impl::StopCapture() {
#ifndef ESP_PLATFORM
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
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
    const bool stopped = done_cv_.wait_for(lock, std::chrono::milliseconds(500),
                                           [this]() { return capture_task_ == nullptr && delivery_task_ == nullptr; });
    if (!stopped) {
        return detail::Unavailable("等待 I2S 采集任务退出超时");
    }
    if (assembler_) {
        assembler_->Reset();
    }
    return Status::Ok();
#endif
}

Status Esp32s3PcmAudioPorts::Impl::CloseInput() {
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

Status Esp32s3PcmAudioPorts::Impl::PushOutput(const voice::AudioFrame& frame) {
#ifndef ESP_PLATFORM
    (void)frame;
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!output_open_ || !playback_format_.has_value()) {
        return detail::Unavailable("输出端口尚未打开");
    }
    if (frame.payload.empty() || frame.payload.size() % (sizeof(int16_t) * playback_format_->channels) != 0) {
        return detail::Invalid("播放帧 PCM 负载无效");
    }
    // 采样率不匹配（如服务端协商 16k、播放端 24k）：线性重采样到播放格式。
    voice::AudioFrame out = frame;
    if (frame.format.sample_rate_hz != 0 && frame.format.sample_rate_hz != playback_format_->sample_rate_hz &&
        frame.format.channels == playback_format_->channels && frame.format.bits_per_sample == 16) {
        const uint32_t src_rate = frame.format.sample_rate_hz;
        const uint32_t dst_rate = playback_format_->sample_rate_hz;
        const std::size_t src_samples = frame.payload.size() / sizeof(int16_t);
        const std::size_t dst_samples = src_samples * dst_rate / src_rate;
        std::vector<uint8_t> resampled(dst_samples * sizeof(int16_t));
        const int16_t* src = reinterpret_cast<const int16_t*>(frame.payload.data());
        auto* dst = reinterpret_cast<int16_t*>(resampled.data());
        for (std::size_t i = 0; i < dst_samples; ++i) {
            const double pos = static_cast<double>(i) * src_rate / dst_rate;
            const std::size_t i0 = static_cast<std::size_t>(pos);
            const std::size_t i1 = i0 + 1 < src_samples ? i0 + 1 : i0;
            const double frac = pos - static_cast<double>(i0);
            dst[i] = static_cast<int16_t>(src[i0] * (1.0 - frac) + src[i1] * frac);
        }
        out.payload = std::move(resampled);
        out.format = *playback_format_;
        ++resampled_frames_;
    } else if (!detail::SameFormat(frame.format, *playback_format_, false)) {
        return detail::Invalid("播放帧格式不支持");
    }
    if (output_queue_.size() >= options_.output_queue_depth) {
        ++rejected_output_frames_;
        return Status::Error(ErrorCode::kConflict, "播放队列已满，拒绝新帧");
    }
    output_queue_.push_back(std::move(out));
    output_high_watermark_.store(std::max(output_high_watermark_.load(), output_queue_.size()));
    output_cv_.notify_one();
    return Status::Ok();
#endif
}

Status Esp32s3PcmAudioPorts::Impl::FlushOutput() {
#ifdef ESP_PLATFORM
    if (amplifier_callback_) {
        amplifier_callback_(false);  // 播放打断/清空：经板级仲裁请求关闭功放。
    }
    std::lock_guard<std::mutex> lock(mutex_);
    output_queue_.clear();
    return Status::Ok();
#else
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#endif
}

bool Esp32s3PcmAudioPorts::Impl::OutputIdle() const {
#ifdef ESP_PLATFORM
    std::lock_guard<std::mutex> lock(mutex_);
    // 播放排空 = 软件队列空 且 无正在写 I2S 的帧（同步阻塞写窗口）。
    return output_queue_.empty() && !output_writing_;
#else
    return true;
#endif
}

Status Esp32s3PcmAudioPorts::Impl::CloseOutput() {
#ifdef ESP_PLATFORM
    if (amplifier_callback_) {
        amplifier_callback_(false);  // 输出关闭：请求关闭功放。
    }
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
    const bool stopped =
        done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() { return output_task_ == nullptr; });
    output_open_ = false;
    playback_format_.reset();
    if (!input_open_) {
        DestroyChannelsLocked();
    }
    return stopped ? Status::Ok() : detail::Unavailable("等待 I2S 播放任务退出超时");
#else
    output_open_ = false;
    playback_format_.reset();
    output_queue_.clear();
    return detail::Unavailable("ESP32-S3 PCM Audio Port 只能在 ESP-IDF 目标运行");
#endif
}

Esp32s3PcmAudioPorts::Esp32s3PcmAudioPorts(AudioBoardProfile profile, AudioPortOptions options,
                                           AmplifierCallback amplifier_callback)
    : impl_(std::make_unique<Impl>(std::move(profile), options, std::move(amplifier_callback))) {}

Esp32s3PcmAudioPorts::~Esp32s3PcmAudioPorts() = default;

voice::AudioInputPort& Esp32s3PcmAudioPorts::input() { return impl_->input(); }

voice::AudioOutputPort& Esp32s3PcmAudioPorts::output() { return impl_->output(); }

AudioPortStats Esp32s3PcmAudioPorts::stats() const { return impl_->stats(); }

void Esp32s3PcmAudioPorts::SetOutputVolume(uint8_t volume) { impl_->SetOutputVolume(volume); }

uint8_t Esp32s3PcmAudioPorts::output_volume() const { return impl_->output_volume(); }

}  // namespace voicelife::audio_esp
