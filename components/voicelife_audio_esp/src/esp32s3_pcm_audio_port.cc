#include <memory>
#include <string>
#include <utility>

#include "es8311_codec_control.h"
#include "esp32s3_pcm_audio_port_internal.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
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
    const uint64_t payload_bytes = static_cast<uint64_t>(negotiated.sample_rate_hz) * negotiated.frame_duration_ms *
                                   negotiated.channels * (negotiated.bits_per_sample / 8U) / 1000U;
    if (payload_bytes == 0 || payload_bytes > voice::AudioFrame::kMaxPayloadBytes) {
        return Invalid("协商下行 PCM 单帧超过允许的负载上限");
    }
    return Status::Ok();
}

uint64_t PcmDurationMs(const voice::AudioFrame& frame) {
    if (frame.format.sample_rate_hz == 0 || frame.format.channels == 0 || frame.format.bits_per_sample == 0 ||
        frame.format.bits_per_sample % 8 != 0) {
        return 0;
    }
    const uint64_t bytes_per_sample = frame.format.bits_per_sample / 8U;
    const uint64_t bytes_per_frame = bytes_per_sample * frame.format.channels;
    if (bytes_per_frame == 0 || frame.payload.size() % bytes_per_frame != 0) {
        return 0;
    }
    const uint64_t samples = frame.payload.size() / bytes_per_frame;
    return std::max<uint64_t>(1, samples * 1000U / frame.format.sample_rate_hz);
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

Status Esp32s3PcmAudioPorts::Impl::OutputPort::Push(voice::AudioFrame frame) {
    return owner_.PushOutput(std::move(frame));
}

Status Esp32s3PcmAudioPorts::Impl::OutputPort::Flush() { return owner_.FlushOutput(); }

bool Esp32s3PcmAudioPorts::Impl::OutputPort::IsIdle() const { return owner_.OutputIdle(); }

void Esp32s3PcmAudioPorts::Impl::OutputPort::Close() { (void)owner_.CloseOutput(); }

Esp32s3PcmAudioPorts::Impl::~Impl() {
    (void)CloseInput();
    (void)CloseOutput();
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
    result.input_pcm_bytes = input_pcm_bytes_.load();
    result.output_pcm_bytes = output_pcm_bytes_.load();
    result.input_samples = input_samples_.load();
    result.input_sum_squares = input_sum_squares_.load();
    result.output_samples = output_samples_.load();
    result.output_sum_squares = output_sum_squares_.load();
    result.input_peak = input_peak_.load();
    result.output_peak = output_peak_.load();
    result.input_zero_periods = input_zero_periods_.load();
    result.output_zero_periods = output_zero_periods_.load();
    result.output_clipped_samples = output_clipped_samples_.load();
    result.input_i2s_errors = input_i2s_errors_.load();
    result.output_i2s_errors = output_i2s_errors_.load();
    result.output_volume = output_volume_.load();
#ifdef ESP_PLATFORM
    result.minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
#endif
    return result;
}

void Esp32s3PcmAudioPorts::Impl::SetOutputVolume(uint8_t volume) {
    const uint8_t normalized = volume > 100 ? 100 : volume;
    std::lock_guard<std::mutex> lock(mutex_);
    output_volume_.store(normalized);
#ifdef ESP_PLATFORM
    if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex && codec_dev_ != nullptr) {
        (void)SetEs8311OutputVolume(codec_dev_, normalized);
    }
#endif
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
    if (options_.input_queue_depth == 0 || options_.output_queue_depth == 0 ||
        options_.maximum_playback_latency_ms == 0) {
        return detail::Invalid("Audio Port 队列容量和最大播放延迟不能为零");
    }
    auto assembler = std::make_unique<PcmFrameAssembler>(format, profile_.capture_i2s.format.frame_duration_ms);
    const Status assembler_status = assembler->Prepare();
    if (!assembler_status.ok()) {
        return assembler_status;
    }
    capture_format_ = format;
    assembler_ = std::move(assembler);
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
    if (options_.input_queue_depth == 0 || options_.output_queue_depth == 0 ||
        options_.maximum_playback_latency_ms == 0) {
        return detail::Invalid("Audio Port 队列容量和最大播放延迟不能为零");
    }
    const uint64_t nominal_latency_ms =
        static_cast<uint64_t>(options_.output_queue_depth) * profile_.playback_i2s.format.frame_duration_ms;
    if (nominal_latency_ms > options_.maximum_playback_latency_ms) {
        return detail::Invalid("播放队列深度超过最大延迟预算");
    }
    // 播放端固定用板级 Profile 格式，协商/上游帧在 Push 时统一重采样。
    // 这样 I2S 时钟恒定（与 MVP 一致），避免 16k 协商导致硬件无声。
    playback_format_ = profile_.playback_i2s.format;
    const uint16_t scratch_frame_duration_ms = std::max(playback_format_->frame_duration_ms, format.frame_duration_ms);
    const uint64_t output_samples = static_cast<uint64_t>(playback_format_->sample_rate_hz) *
                                    scratch_frame_duration_ms * playback_format_->channels / 1000U;
    const uint64_t wire_slots =
        profile_.playback_i2s.wire_slot_count == 0 ? playback_format_->channels : profile_.playback_i2s.wire_slot_count;
    // 这些缓冲仅由输出任务使用；在 Open 阶段一次性分配，避免首帧 TTS 在
    // I2S 实时路径扩容。Profile 已校验位宽与 slot 数，以下计算仍保留上限保护。
    if (output_samples > 0 && output_samples <= codec_pcm_scratch_.max_size()) {
        codec_pcm_scratch_.reserve(static_cast<std::size_t>(output_samples));
    }
    const uint64_t wire_bytes = output_samples * wire_slots * profile_.playback_i2s.wire_bits_per_sample / 8U;
    if (wire_bytes > 0 && wire_bytes <= wire_scratch_.max_size()) {
        wire_scratch_.reserve(static_cast<std::size_t>(wire_bytes));
    }
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
    if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex && !codec_initialized_) {
        // esp_codec_dev::set_fmt first disables both channels before it reconfigures
        // and enables them again. Prime the pair so that transition is legal and
        // MCLK is already present while the ES8311 starts.
        if (i2s_channel_enable(tx_channel_) != ESP_OK) {
            output_open_ = false;
            playback_format_.reset();
            return detail::Unavailable("启动 ES8311 I2S TX 通道失败");
        }
        if (i2s_channel_enable(rx_channel_) != ESP_OK) {
            (void)i2s_channel_disable(tx_channel_);
            output_open_ = false;
            playback_format_.reset();
            return detail::Unavailable("启动 ES8311 I2S RX 通道失败");
        }
#ifdef ESP_PLATFORM
        ESP_LOGI(voicelife::audio_esp::detail::kAudioRuntimeTag, "ES8311_I2S_PRIMED tx=1 rx=1");
#endif
        const auto& control = *profile_.codec_control;
        Es8311ControlConfig codec_config;
        codec_config.i2c_port = control.i2c_port;
        codec_config.sda_gpio = control.i2c.sda;
        codec_config.scl_gpio = control.i2c.scl;
        codec_config.es8311_8bit = control.addresses.es8311_8bit;
        codec_config.tx_channel = tx_channel_;
        codec_config.rx_channel = rx_channel_;
        codec_config.sample_rate_hz = static_cast<int>(profile_.playback_i2s.format.sample_rate_hz);
        const auto codec_result = InitializeEs8311(codec_config);
        if (!codec_result.ok()) {
            (void)i2s_channel_disable(rx_channel_);
            (void)i2s_channel_disable(tx_channel_);
            output_open_ = false;
            playback_format_.reset();
            return codec_result.status;
        }
        codec_dev_ = codec_result.value.value_or(nullptr);
        codec_initialized_ = true;
    }
    output_running_ = true;
    if (profile_.topology != AudioBoardTopology::kExternalCodecDuplex && i2s_channel_enable(tx_channel_) != ESP_OK) {
        output_running_ = false;
        output_open_ = false;
        playback_format_.reset();
        return detail::Unavailable("启动 I2S 播放通道失败");
    }
    if (amplifier_callback_) {
        amplifier_callback_(true);  // 播放打开：经板级仲裁请求功放。
        amplifier_enabled_ = true;
    }
    if (xTaskCreate(&OutputTaskEntry, "voice_audio_out", 4096, this, 4, &output_task_) != pdPASS) {
        if (amplifier_callback_) {
            amplifier_callback_(false);
            amplifier_enabled_ = false;
        }
        if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex && codec_dev_ != nullptr) {
            (void)DeinitializeEs8311(codec_dev_);
            codec_dev_ = nullptr;
            codec_initialized_ = false;
        } else {
            (void)i2s_channel_disable(tx_channel_);
        }
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
    if (profile_.topology != AudioBoardTopology::kExternalCodecDuplex && i2s_channel_enable(rx_channel_) != ESP_OK) {
        input_running_ = false;
        return detail::Unavailable("启动 I2S 采集通道失败");
    }
    if (xTaskCreate(&CaptureTaskEntry, "voice_audio_in", 4096, this, 5, &capture_task_) != pdPASS) {
        input_running_ = false;
        if (profile_.topology != AudioBoardTopology::kExternalCodecDuplex) i2s_channel_disable(rx_channel_);
        input_cv_.notify_all();
        return detail::Unavailable("创建 I2S 采集任务失败");
    }
    // 投递任务栈常驻 PSRAM、TCB 在内部 RAM：待机恢复时内部 RAM 最大连续块
    // 常 <16KB，16384B 动态栈创建必然失败。xTaskCreateStatic 的栈可放 PSRAM
    // （xPortCheckValidStackMem 允许外部 RAM），但 TCB 必须内部 RAM
    // （xPortCheckValidTCBMem 断言，调度器临界区依赖内部寻址）。一次性分配、
    // 跨采集周期复用，不释放（PSRAM 8MB 充裕，随 AudioPorts 生命周期）。
    constexpr uint32_t kDeliveryStackWords = 16384 / sizeof(StackType_t);
    if (delivery_stack_ == nullptr || delivery_tcb_ == nullptr) {
        delivery_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(sizeof(StackType_t) * kDeliveryStackWords, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        delivery_tcb_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (delivery_stack_ == nullptr || delivery_tcb_ == nullptr) {
            heap_caps_free(delivery_stack_);
            heap_caps_free(delivery_tcb_);
            delivery_stack_ = nullptr;
            delivery_tcb_ = nullptr;
            input_running_ = false;
            if (profile_.topology != AudioBoardTopology::kExternalCodecDuplex) i2s_channel_disable(rx_channel_);
            input_cv_.notify_all();
            const bool capture_stopped =
                done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() { return capture_task_ == nullptr; });
            if (!capture_stopped) {
                return detail::Unavailable("等待 I2S 采集任务退出超时");
            }
            return detail::Unavailable("PSRAM 分配 I2S 音频投递任务失败");
        }
    }
    TaskHandle_t delivery_task = xTaskCreateStatic(&DeliveryTaskEntry, "voice_audio_sink", kDeliveryStackWords, this, 4,
                                                   delivery_stack_, delivery_tcb_);
    if (delivery_task == nullptr) {
        input_running_ = false;
        if (profile_.topology != AudioBoardTopology::kExternalCodecDuplex) i2s_channel_disable(rx_channel_);
        input_cv_.notify_all();
        const bool capture_stopped =
            done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() { return capture_task_ == nullptr; });
        if (!capture_stopped) {
            return detail::Unavailable("等待 I2S 采集任务退出超时");
        }
        return detail::Unavailable("创建 I2S 音频投递任务失败");
    }
    delivery_task_ = delivery_task;
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
        if (profile_.topology != AudioBoardTopology::kExternalCodecDuplex && rx_channel_ != nullptr) {
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

Status Esp32s3PcmAudioPorts::Impl::PushOutput(voice::AudioFrame frame) {
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
    voice::AudioFrame out = std::move(frame);
    if (out.format.sample_rate_hz != 0 && out.format.sample_rate_hz != playback_format_->sample_rate_hz &&
        out.format.channels == playback_format_->channels && out.format.bits_per_sample == 16) {
        const uint32_t src_rate = out.format.sample_rate_hz;
        const uint32_t dst_rate = playback_format_->sample_rate_hz;
        const std::size_t src_samples = out.payload.size() / sizeof(int16_t);
        const std::size_t dst_samples = src_samples * dst_rate / src_rate;
        std::vector<uint8_t> resampled(dst_samples * sizeof(int16_t));
        const int16_t* src = reinterpret_cast<const int16_t*>(out.payload.data());
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
    } else if (!detail::SameFormat(out.format, *playback_format_, false)) {
        return detail::Invalid("播放帧格式不支持");
    }
    const uint64_t frame_duration_ms = detail::PcmDurationMs(out);
    if (frame_duration_ms == 0) {
        return detail::Invalid("播放帧无法推导 PCM 时长");
    }
    if (output_queue_.size() >= options_.output_queue_depth) {
        ++rejected_output_frames_;
        return Status::Error(ErrorCode::kConflict, "播放队列已满，拒绝新帧");
    }
    if (output_queue_duration_ms_ + frame_duration_ms > options_.maximum_playback_latency_ms) {
        ++rejected_output_frames_;
        return Status::Error(ErrorCode::kConflict, "播放队列超过最大延迟预算，拒绝新帧");
    }
    output_queue_.push_back(std::move(out));
    output_queue_duration_ms_ += frame_duration_ms;
    output_high_watermark_.store(std::max(output_high_watermark_.load(), output_queue_.size()));
    amplifier_disable_pending_ = false;
    if (!amplifier_enabled_ && amplifier_callback_) {
        amplifier_callback_(true);
        amplifier_enabled_ = true;
    }
    output_cv_.notify_one();
    return Status::Ok();
#endif
}

Status Esp32s3PcmAudioPorts::Impl::FlushOutput() {
#ifdef ESP_PLATFORM
    std::lock_guard<std::mutex> lock(mutex_);
    output_queue_.clear();
    output_queue_duration_ms_ = 0;
    if (output_writing_) {
        amplifier_disable_pending_ = true;
    } else if (amplifier_enabled_ && amplifier_callback_) {
        amplifier_callback_(false);  // 播放打断/清空：经板级仲裁请求关闭功放。
        amplifier_enabled_ = false;
    }
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_writing_) {
            amplifier_disable_pending_ = true;
        } else if (amplifier_enabled_ && amplifier_callback_) {
            amplifier_callback_(false);  // 输出关闭：请求关闭功放。
            amplifier_enabled_ = false;
        }
        if (output_open_) {
            output_running_ = false;
            output_queue_.clear();
            output_queue_duration_ms_ = 0;
            output_cv_.notify_all();
        }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool stopped =
        done_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() { return output_task_ == nullptr; });
    if (!stopped) {
        return detail::Unavailable("等待 I2S 播放任务退出超时");
    }
    lock.unlock();

    // No PCM writer may race the codec data interface while it disables the
    // full-duplex channels. The managed codec owns those channels after Open.
    if (profile_.topology == AudioBoardTopology::kExternalCodecDuplex && codec_dev_ != nullptr) {
        (void)DeinitializeEs8311(codec_dev_);
        codec_dev_ = nullptr;
        codec_initialized_ = false;
    } else if (tx_channel_ != nullptr) {
        (void)i2s_channel_disable(tx_channel_);
    }

    lock.lock();
    output_open_ = false;
    playback_format_.reset();
    if (!input_open_) {
        DestroyChannelsLocked();
    }
    return Status::Ok();
#else
    output_open_ = false;
    playback_format_.reset();
    output_queue_.clear();
    output_queue_duration_ms_ = 0;
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
