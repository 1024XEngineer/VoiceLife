#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/pcm_frame_assembler.h"

#ifdef ESP_PLATFORM

#include "driver/i2s_std.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#endif

namespace voicelife::audio_esp {
namespace detail {

Status Invalid(std::string message);
Status Unavailable(std::string message);

#ifdef ESP_PLATFORM
bool SameFormat(const voice::AudioFormat& left, const voice::AudioFormat& right, bool include_frame_duration);
size_t WireBytes(const I2sEndpointProfile& endpoint);
i2s_data_bit_width_t WireWidth(const I2sEndpointProfile& endpoint);
i2s_std_config_t MakeStdConfig(const I2sEndpointProfile& endpoint, bool tx);
int16_t ToPcm16(int32_t raw, const I2sEndpointProfile& endpoint);
int32_t ToWire(int16_t pcm, const I2sEndpointProfile& endpoint);
#endif

Status ValidateNegotiatedFormat(const I2sEndpointProfile& endpoint, const voice::AudioFormat& negotiated);

}  // namespace detail

class Esp32s3PcmAudioPorts::Impl final {
   public:
    class InputPort final : public voice::AudioInputPort {
       public:
        explicit InputPort(Impl& owner) : owner_(owner) {}
        void SetAudioSink(voice::AudioFrameSink sink) override;
        Status Open(const voice::AudioFormat& format) override;
        Status StartCapture(voice::VoiceMode mode) override;
        Status StopCapture() override;
        void Close() override;

       private:
        Impl& owner_;
    };

    class OutputPort final : public voice::AudioOutputPort {
       public:
        explicit OutputPort(Impl& owner) : owner_(owner) {}
        Status Open(const voice::AudioFormat& format) override;
        Status Push(const voice::AudioFrame& frame) override;
        Status Flush() override;
        void Close() override;

       private:
        Impl& owner_;
    };

    Impl(AudioBoardProfile profile, AudioPortOptions options)
        : profile_(std::move(profile)), options_(options), input_port_(*this), output_port_(*this) {}

    ~Impl();

    InputPort& input() { return input_port_; }
    OutputPort& output() { return output_port_; }

    AudioPortStats stats() const;

   private:
    friend class InputPort;
    friend class OutputPort;

    Status OpenInput(const voice::AudioFormat& format);
    Status OpenOutput(const voice::AudioFormat& format);
    Status StartCapture(voice::VoiceMode mode);
    Status StopCapture();
    Status CloseInput();
    Status PushOutput(const voice::AudioFrame& frame);
    Status FlushOutput();
    Status CloseOutput();

#ifdef ESP_PLATFORM
    void EnqueueInput(voice::AudioFrame frame);
    Status TryInitializeChannelsLocked();
    void DestroyChannels();
    void DestroyChannelsLocked();
    static void CaptureTaskEntry(void* arg);
    static void DeliveryTaskEntry(void* arg);
    static void OutputTaskEntry(void* arg);
    void MarkTaskDone(TaskHandle_t* task);
    void CaptureLoop();
    void DeliveryLoop();
    Status WriteFrame(const voice::AudioFrame& frame);
    void OutputLoop();
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

}  // namespace voicelife::audio_esp
