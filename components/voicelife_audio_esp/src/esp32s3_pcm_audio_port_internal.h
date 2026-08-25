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
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#endif

namespace voicelife::audio_esp {
namespace detail {

constexpr char kAudioRuntimeTag[] = "VoiceLifeAudioRuntime";

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
Status ValidatePlaybackFormat(const I2sEndpointProfile& endpoint, const voice::AudioFormat& negotiated);
uint64_t PcmDurationMs(const voice::AudioFrame& frame);

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
        Status DiscardPendingInput() override;
        void Close() override;

       private:
        Impl& owner_;
    };

    class OutputPort final : public voice::AudioOutputPort {
       public:
        explicit OutputPort(Impl& owner) : owner_(owner) {}
        Status Open(const voice::AudioFormat& format) override;
        Status Push(voice::AudioFrame frame) override;
        Status Flush() override;
        bool IsIdle() const override;
        void Close() override;

       private:
        Impl& owner_;
    };

    using AmplifierCallback = std::function<void(bool)>;

    Impl(AudioBoardProfile profile, AudioPortOptions options, AmplifierCallback amplifier_callback)
        : profile_(std::move(profile)),
          options_(options),
          input_port_(*this),
          output_port_(*this),
          amplifier_callback_(std::move(amplifier_callback)) {}

    ~Impl();

    InputPort& input() { return input_port_; }
    OutputPort& output() { return output_port_; }

    AudioPortStats stats() const;
    void SetOutputVolume(uint8_t volume);
    uint8_t output_volume() const { return output_volume_.load(); }
    Status SetTestInputEnabled(bool enabled);
    Status InjectTestInput(voice::AudioFrame frame);

   private:
    friend class InputPort;
    friend class OutputPort;

    Status OpenInput(const voice::AudioFormat& format);
    Status OpenOutput(const voice::AudioFormat& format);
    Status StartCapture(voice::VoiceMode mode);
    Status StopCapture();
    Status DiscardPendingInput();
    Status CloseInput();
    Status PushOutput(voice::AudioFrame frame);
    Status FlushOutput();
    bool OutputIdle() const;
    Status CloseOutput();

#ifdef ESP_PLATFORM
    void EnqueueInput(voice::AudioFrame frame);
    void EnqueueInputLocked(voice::AudioFrame frame);
    Status TryInitializeChannelsLocked();
    void DestroyChannels();
    void DestroyChannelsLocked();
    void ReleaseTaskStorage();
    void WaitForInputTasks();
    void WaitForOutputTask();
    static void CaptureTaskEntry(void* arg);
    static void DeliveryTaskEntry(void* arg);
    static void OutputTaskEntry(void* arg);
    void MarkTaskDone(TaskHandle_t* task);
    void CaptureLoop();
    void DeliveryLoop();
    Status WriteFrame(const voice::AudioFrame& frame);
    void OutputLoop();
    Status FinalizeOutputClose();
#endif

    AudioBoardProfile profile_;
    AudioPortOptions options_;
    InputPort input_port_;
    OutputPort output_port_;
    /** @brief 功放请求回调（经板级仲裁，不得直接写 GPIO）。 */
    std::function<void(bool)> amplifier_callback_;
    /** @brief ES8311 是否已初始化（duplex 首次打开时）。 */
    [[maybe_unused]] bool codec_initialized_ = false;
    /** @brief ES8311 Codec 设备句柄（归属本 AudioPorts，Close 时释放）。 */
    [[maybe_unused]] void* codec_dev_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable input_cv_;
    std::condition_variable output_cv_;
    std::condition_variable output_space_cv_;
    std::condition_variable done_cv_;
    std::deque<voice::AudioFrame> input_queue_;
    std::deque<voice::AudioFrame> output_queue_;
    uint64_t output_queue_duration_ms_ = 0;
    // Reused only by the output task to avoid heap churn for every I2S period.
    std::vector<int16_t> codec_pcm_scratch_;
    std::vector<uint8_t> wire_scratch_;
    voice::AudioFrameSink input_sink_;
    std::optional<voice::AudioFormat> capture_format_;
    std::optional<voice::AudioFormat> playback_format_;
    std::unique_ptr<PcmFrameAssembler> assembler_;
    bool input_open_ = false;
    bool output_open_ = false;
#ifdef ESP_PLATFORM
    bool output_closing_ = false;
    // A timeout may outlive the caller. Either CloseOutput or the late output
    // task exit owns final cleanup, never both.
    bool output_cleanup_started_ = false;
    // A barge-in flush powers down the amplifier. The next accepted TTS frame
    // must restore that request before it reaches I2S.
    bool amplifier_enabled_ = false;
#endif
#ifdef ESP_PLATFORM
    bool channels_ready_ = false;
    bool input_running_ = false;
    bool output_running_ = false;
    // 正在执行 i2s_channel_write 的帧（同步阻塞写期间队列可能空但 I2S 仍在播）。
    bool output_writing_ = false;
    bool amplifier_disable_pending_ = false;
#endif

    std::atomic<std::size_t> captured_frames_{0};
    std::atomic<std::size_t> dropped_input_frames_{0};
    std::atomic<std::size_t> played_frames_{0};
    std::atomic<std::size_t> rejected_output_frames_{0};
    std::atomic<std::size_t> resampled_frames_{0};
    std::atomic<std::size_t> short_reads_{0};
    std::atomic<std::size_t> short_writes_{0};
    std::atomic<std::size_t> input_high_watermark_{0};
    std::atomic<std::size_t> output_high_watermark_{0};
    std::atomic<uint8_t> output_volume_{70};
    std::atomic<uint64_t> input_pcm_bytes_{0};
    std::atomic<uint64_t> output_pcm_bytes_{0};
    std::atomic<uint64_t> input_samples_{0};
    std::atomic<uint64_t> input_sum_squares_{0};
    std::atomic<uint64_t> output_samples_{0};
    std::atomic<uint64_t> output_sum_squares_{0};
    std::atomic<uint16_t> input_peak_{0};
    std::atomic<uint16_t> output_peak_{0};
    std::atomic<uint64_t> input_zero_periods_{0};
    std::atomic<uint64_t> output_zero_periods_{0};
    std::atomic<uint64_t> output_clipped_samples_{0};
    std::atomic<uint64_t> input_i2s_errors_{0};
    std::atomic<uint64_t> output_i2s_errors_{0};
    std::atomic<uint64_t> test_injected_input_frames_{0};
    std::atomic<uint64_t> test_injected_input_bytes_{0};
    std::atomic_bool test_input_enabled_{false};

#ifdef ESP_PLATFORM
    i2s_chan_handle_t tx_channel_ = nullptr;
    i2s_chan_handle_t rx_channel_ = nullptr;
    TaskHandle_t capture_task_ = nullptr;
    TaskHandle_t delivery_task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;
    // 采集/投递任务栈常驻 PSRAM、TCB 在内部 RAM（一次性分配、跨采集周期
    // 复用）：待机恢复时内部 RAM 最大连续块常 <16KB，动态任务栈会创建失败。
    // TCB 必须留在内部 RAM（xPortCheckValidTCBMem 断言）；所有存储在析构时
    // 确认任务退出后释放。
    StackType_t* capture_stack_ = nullptr;
    StaticTask_t* capture_tcb_ = nullptr;
    StackType_t* delivery_stack_ = nullptr;
    StaticTask_t* delivery_tcb_ = nullptr;
#else
    void DestroyChannels() {}
#endif
};

}  // namespace voicelife::audio_esp
