#include "serial_voice_test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "serial_voice_protocol.h"

#ifdef ESP_PLATFORM
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "SerialVoiceTest";
Status Unavailable(const char* message) { return Status::Error(ErrorCode::kUnavailable, message); }

}  // namespace

class SerialVoiceTest::Impl final {
   public:
    explicit Impl(SerialVoiceTestCallbacks callbacks) : callbacks_(std::move(callbacks)) {}

    Status Start() {
#ifndef ESP_PLATFORM
        return Unavailable("串口语音测试只能在 ESP-IDF 目标运行");
#else
        if (task_ != nullptr) return Status::Ok();
        if (!callbacks_.begin_turn || !callbacks_.submit_pcm || !callbacks_.end_turn || !callbacks_.begin_wake ||
            !callbacks_.end_wake) {
            return Status::Error(ErrorCode::kInvalidArgument, "串口语音测试回调不完整");
        }
        payload_pool_ = voice::AudioPayloadPool::Create(16, detail::kSerialVoicePcmBytes);
        if (payload_pool_ == nullptr) return Unavailable("创建串口语音 payload pool 失败");
        if (!usb_serial_jtag_is_driver_installed()) {
            usb_serial_jtag_driver_config_t config = {
                .tx_buffer_size = 1024,
                .rx_buffer_size = 2048,
            };
            if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
                return Unavailable("初始化 USB 串口语音测试驱动失败");
            }
        }
        stopping_.store(false);
        TaskHandle_t created_task = nullptr;
        if (xTaskCreate(&TaskEntry, "serial_voice_test", 4096, this, 3, &created_task) != pdPASS) {
            task_.store(nullptr);
            return Unavailable("创建串口语音测试任务失败");
        }
        task_.store(created_task);
        return Status::Ok();
#endif
    }

    void Stop() {
#ifdef ESP_PLATFORM
        stopping_.store(true);
        const TaskHandle_t task = task_.load();
        // Callbacks normally run outside this task, but a self-stop must not
        // wait for the current task to return.
        if (task == nullptr || task == xTaskGetCurrentTaskHandle()) return;
        while (task_.load() != nullptr) vTaskDelay(1);
#endif
    }

   private:
#ifdef ESP_PLATFORM
    static void TaskEntry(void* context) {
        static_cast<Impl*>(context)->Run();
        vTaskDelete(nullptr);
    }

    bool ReadByte(uint8_t* destination) { return usb_serial_jtag_read_bytes(destination, 1, pdMS_TO_TICKS(100)) == 1; }

    bool ReadExact(uint8_t* destination, std::size_t size) {
        std::size_t received = 0;
        while (received < size && !stopping_.load()) {
            const int count = usb_serial_jtag_read_bytes(destination + received, size - received, pdMS_TO_TICKS(100));
            if (count > 0) received += static_cast<std::size_t>(count);
        }
        return received == size;
    }

    bool DiscardExact(std::size_t size) {
        std::array<uint8_t, 64> discard{};
        while (size != 0) {
            const std::size_t chunk = std::min(size, discard.size());
            if (!ReadExact(discard.data(), chunk)) return false;
            size -= chunk;
        }
        return true;
    }

    void LogResult(const char* event, const Status& status) {
        if (status.ok()) {
            ESP_LOGI(kTag, "SERIAL_VOICE_%s=ok", event);
        } else {
            ESP_LOGW(kTag, "SERIAL_VOICE_%s=reject code=%d", event, static_cast<int>(status.code));
        }
    }

    void Run() {
        ESP_LOGI(kTag, "SERIAL_VOICE_TEST_READY=1 protocol=VLVT-v1 pcm=s16le-16000-mono-20ms payload_bytes=%u",
                 static_cast<unsigned>(detail::kSerialVoicePcmBytes));
        detail::SerialVoiceMagicMatcher magic;
        while (!stopping_.load()) {
            uint8_t byte = 0;
            if (!ReadByte(&byte)) continue;
            if (!magic.Push(byte)) continue;

            std::array<uint8_t, 4> header{};
            if (!ReadExact(header.data(), header.size())) continue;
            const detail::SerialVoiceFrameHeader frame_header{
                .version = header[0],
                .kind = header[1],
                .payload_bytes =
                    static_cast<uint16_t>(static_cast<uint16_t>(header[2]) | (static_cast<uint16_t>(header[3]) << 8U)),
            };
            if (!detail::IsValidSerialVoiceHeader(frame_header)) {
                ESP_LOGW(kTag, "SERIAL_VOICE_FRAME_REJECT version=%u kind=%u length=%u",
                         static_cast<unsigned>(frame_header.version), static_cast<unsigned>(frame_header.kind),
                         static_cast<unsigned>(frame_header.payload_bytes));
                (void)DiscardExact(frame_header.payload_bytes);
                continue;
            }
            if (frame_header.kind == detail::kSerialVoiceBegin) {
                LogResult("TURN_BEGIN", callbacks_.begin_turn());
                continue;
            }
            if (frame_header.kind == detail::kSerialVoiceEnd) {
                LogResult("TURN_END", callbacks_.end_turn());
                continue;
            }
            if (frame_header.kind == detail::kSerialVoiceWakeBegin) {
                LogResult("WAKE_BEGIN", callbacks_.begin_wake());
                continue;
            }
            if (frame_header.kind == detail::kSerialVoiceWakeEnd) {
                LogResult("WAKE_END", callbacks_.end_wake());
                continue;
            }
            if (frame_header.kind != detail::kSerialVoicePcm) {
                ESP_LOGW(kTag, "SERIAL_VOICE_FRAME_REJECT unknown_kind=%u", static_cast<unsigned>(frame_header.kind));
                continue;
            }
            std::array<uint8_t, detail::kSerialVoicePcmBytes> payload{};
            if (!ReadExact(payload.data(), payload.size())) continue;
            voice::AudioFrame frame;
            frame.format = {.codec = voice::AudioCodec::kPcmS16Le,
                            .sample_rate_hz = 16000,
                            .channels = 1,
                            .bits_per_sample = 16,
                            .frame_duration_ms = 20};
            frame.payload = payload_pool_->TryAcquire();
            if (!frame.payload.pooled()) {
                ESP_LOGW(kTag, "SERIAL_VOICE_PCM=reject code=%d", static_cast<int>(ErrorCode::kUnavailable));
                continue;
            }
            std::memcpy(frame.payload.data(), payload.data(), payload.size());
            const Status status = callbacks_.submit_pcm(std::move(frame));
            if (!status.ok()) LogResult("PCM", status);
        }
        task_.store(nullptr);
    }
#endif

    SerialVoiceTestCallbacks callbacks_;
    std::atomic_bool stopping_{false};
    std::shared_ptr<voice::AudioPayloadPool> payload_pool_;
#ifdef ESP_PLATFORM
    std::atomic<TaskHandle_t> task_{nullptr};
#endif
};

SerialVoiceTest::SerialVoiceTest(SerialVoiceTestCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}

SerialVoiceTest::~SerialVoiceTest() { impl_->Stop(); }

Status SerialVoiceTest::Start() { return impl_->Start(); }

void SerialVoiceTest::Stop() { impl_->Stop(); }

}  // namespace voicelife::runtime
