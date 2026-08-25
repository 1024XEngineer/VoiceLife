#include "serial_voice_test.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "serial_voice_protocol.h"
#include "usb_serial_frame_router.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#endif

namespace voicelife::runtime {
namespace {

constexpr char kTag[] = "SerialVoiceTest";
// Serial PCM arrives from a host-side real-time fixture. Unlike the I2S
// capture task, this dedicated test task may briefly wait for the bounded
// transport backlog to drain, preserving the exact source utterance instead
// of manufacturing a lost-frame failure during a TLS stall.
constexpr uint32_t kPayloadAcquireTimeoutMs = 2000;
constexpr uint32_t kPayloadAcquirePollMs = 5;
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
        // The Linx TX worker can briefly retain more than 16 input frames while
        // TLS is flushing. Keep the serial fixture from rejecting valid PCM
        // merely because the network is momentarily slower than realtime.
        payload_pool_ = voice::AudioPayloadPool::Create(32, detail::kSerialVoicePcmBytes);
        if (payload_pool_ == nullptr) return Unavailable("创建串口语音 payload pool 失败");
        if (const Status router_status = StartUsbSerialFrameRouter(); !router_status.ok()) return router_status;
        stopping_.store(false);
        TaskHandle_t created_task = nullptr;
        // The harness starts after the production voice stack is ready, when TLS/MCP/audio startup may
        // have fragmented internal RAM. It performs no cache-disabled work, so keep its stack in PSRAM.
        if (xTaskCreateWithCaps(&TaskEntry, "serial_voice_test", 4096, this, 3, &created_task,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
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
        vTaskDeleteWithCaps(nullptr);
    }

    void LogResult(const char* event, const Status& status) {
        if (status.ok()) {
            ESP_LOGI(kTag, "SERIAL_VOICE_%s=ok", event);
        } else {
            ESP_LOGW(kTag, "SERIAL_VOICE_%s=reject code=%d", event, static_cast<int>(status.code));
        }
    }

    voice::AudioPayload AcquirePcmPayload() {
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kPayloadAcquireTimeoutMs);
        while (!stopping_.load()) {
            voice::AudioPayload payload = payload_pool_->TryAcquire();
            if (payload.pooled()) return payload;
            if (xTaskGetTickCount() >= deadline) break;
            vTaskDelay(pdMS_TO_TICKS(kPayloadAcquirePollMs));
        }
        return {};
    }

    void Run() {
        ESP_LOGI(kTag, "SERIAL_VOICE_TEST_READY=1 protocol=VLVT-v1 pcm=s16le-16000-mono-20ms payload_bytes=%u",
                 static_cast<unsigned>(detail::kSerialVoicePcmBytes));
        while (!stopping_.load()) {
            UsbSerialFrame usb_frame;
            if (!ReceiveSerialVoiceUsbFrame(&usb_frame, 100)) continue;
            if (usb_frame.size < 8) continue;
            const detail::SerialVoiceFrameHeader frame_header{
                .version = usb_frame.bytes[4],
                .kind = usb_frame.bytes[5],
                .payload_bytes = static_cast<uint16_t>(static_cast<uint16_t>(usb_frame.bytes[6]) |
                                                       (static_cast<uint16_t>(usb_frame.bytes[7]) << 8U)),
            };
            if (!detail::IsValidSerialVoiceHeader(frame_header) || usb_frame.size != 8 + frame_header.payload_bytes) {
                ESP_LOGW(kTag, "SERIAL_VOICE_FRAME_REJECT version=%u kind=%u length=%u",
                         static_cast<unsigned>(frame_header.version), static_cast<unsigned>(frame_header.kind),
                         static_cast<unsigned>(frame_header.payload_bytes));
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
            voice::AudioFrame frame;
            frame.format = {.codec = voice::AudioCodec::kPcmS16Le,
                            .sample_rate_hz = 16000,
                            .channels = 1,
                            .bits_per_sample = 16,
                            .frame_duration_ms = 20};
            frame.payload = AcquirePcmPayload();
            if (!frame.payload.pooled()) {
                ESP_LOGW(kTag, "SERIAL_VOICE_PCM=reject code=%d reason=payload_backpressure_timeout",
                         static_cast<int>(ErrorCode::kUnavailable));
                continue;
            }
            std::memcpy(frame.payload.data(), usb_frame.bytes.data() + 8, detail::kSerialVoicePcmBytes);
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
