#include "usb_serial_frame_router.h"

#include <algorithm>
#include <array>
#include <atomic>

#include "serial_voice_protocol.h"
#include "voicelife/im/im_provisioning.h"

#ifdef ESP_PLATFORM
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

namespace voicelife::runtime {
namespace {

constexpr std::array<uint8_t, 4> kImProvisioningV1Magic = {'V', 'L', 'I', '1'};
constexpr std::array<uint8_t, 4> kImProvisioningV2Magic = {'V', 'L', 'I', '2'};
constexpr std::array<uint8_t, 4> kImPairingMagic = {'V', 'L', 'P', '1'};
constexpr std::size_t kSerialVoiceHeaderSize = 8;

bool MatchesPrefix(std::span<const uint8_t> candidate, std::span<const uint8_t> magic) {
    return candidate.size() <= magic.size() && std::equal(candidate.begin(), candidate.end(), magic.begin());
}

std::optional<UsbSerialFrameKind> FullMagicKind(std::span<const uint8_t> magic) {
    if (std::equal(magic.begin(), magic.end(), kImProvisioningV1Magic.begin()))
        return UsbSerialFrameKind::kImProvisioning;
    if (std::equal(magic.begin(), magic.end(), kImProvisioningV2Magic.begin()))
        return UsbSerialFrameKind::kImProvisioning;
    if (std::equal(magic.begin(), magic.end(), kImPairingMagic.begin())) return UsbSerialFrameKind::kImPairing;
    if (std::equal(magic.begin(), magic.end(), detail::kSerialVoiceMagic.begin()))
        return UsbSerialFrameKind::kSerialVoice;
    return std::nullopt;
}

#ifdef ESP_PLATFORM
constexpr char kTag[] = "UsbFrameRouter";
constexpr UBaseType_t kImQueueDepth = 4;
constexpr UBaseType_t kVoiceQueueDepth = 4;
std::atomic_bool g_started{false};
QueueHandle_t g_im_queue = nullptr;
QueueHandle_t g_voice_queue = nullptr;

void RouterTask(void*) {
    UsbSerialFrameDecoder decoder;
    while (true) {
        uint8_t byte = 0;
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100)) != 1) continue;
        auto frame = decoder.Push(byte);
        if (!frame.has_value()) continue;
        QueueHandle_t target = frame->kind == UsbSerialFrameKind::kSerialVoice ? g_voice_queue : g_im_queue;
        if (target == nullptr || xQueueSend(target, &*frame, 0) != pdTRUE) {
            ESP_LOGW(kTag, "USB_SERIAL_FRAME_DROP kind=%u", static_cast<unsigned>(frame->kind));
        }
        std::fill(frame->bytes.begin(), frame->bytes.end(), 0);
    }
}

bool Receive(QueueHandle_t queue, UsbSerialFrame* destination, int timeout_ms) {
    if (queue == nullptr || destination == nullptr || timeout_ms < 0) return false;
    return xQueueReceive(queue, destination, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
#endif

}  // namespace

void UsbSerialFrameDecoder::Reset() {
    std::fill(frame_.bytes.begin(), frame_.bytes.end(), 0);
    frame_.size = 0;
    expected_size_ = 0;
    state_ = State::kSearchingMagic;
}

bool UsbSerialFrameDecoder::IsMagicPrefix() const {
    const auto candidate = std::span<const uint8_t>(frame_.bytes.data(), frame_.size);
    return MatchesPrefix(candidate, kImProvisioningV1Magic) || MatchesPrefix(candidate, kImProvisioningV2Magic) ||
           MatchesPrefix(candidate, kImPairingMagic) || MatchesPrefix(candidate, detail::kSerialVoiceMagic);
}

std::optional<UsbSerialFrame> UsbSerialFrameDecoder::FinishFrame() {
    UsbSerialFrame complete = frame_;
    Reset();
    return complete;
}

std::optional<UsbSerialFrame> UsbSerialFrameDecoder::Push(uint8_t byte) {
    if (state_ == State::kSearchingMagic) {
        if (frame_.size == 4) Reset();
        frame_.bytes[frame_.size++] = byte;
        while (frame_.size != 0 && !IsMagicPrefix()) {
            std::move(frame_.bytes.begin() + 1, frame_.bytes.begin() + frame_.size, frame_.bytes.begin());
            --frame_.size;
        }
        if (frame_.size != 4) return std::nullopt;

        const auto kind = FullMagicKind(std::span<const uint8_t>(frame_.bytes.data(), frame_.size));
        if (!kind.has_value()) {
            Reset();
            return std::nullopt;
        }
        frame_.kind = *kind;
        expected_size_ =
            frame_.kind == UsbSerialFrameKind::kSerialVoice ? kSerialVoiceHeaderSize : im::kImProvisioningHeaderSize;
        state_ = State::kReadingFrame;
        return std::nullopt;
    }

    if (frame_.size == frame_.bytes.size()) {
        Reset();
        return std::nullopt;
    }
    frame_.bytes[frame_.size++] = byte;
    if (frame_.size != expected_size_) return std::nullopt;

    if (frame_.kind == UsbSerialFrameKind::kImPairing) {
        if (!im::ParseImPairingTrigger(frame_.view()).ok()) {
            Reset();
            return std::nullopt;
        }
        return FinishFrame();
    }
    if (frame_.kind == UsbSerialFrameKind::kImProvisioning) {
        if (expected_size_ != im::kImProvisioningHeaderSize) return FinishFrame();
        const auto header = im::ParseImProvisioningHeader(frame_.view());
        if (!header.ok() || !header.value.has_value() ||
            frame_.size + header.value->payload_size > frame_.bytes.size()) {
            Reset();
            return std::nullopt;
        }
        expected_size_ += header.value->payload_size;
        return expected_size_ == frame_.size ? FinishFrame() : std::nullopt;
    }

    const detail::SerialVoiceFrameHeader header{
        .version = frame_.bytes[4],
        .kind = frame_.bytes[5],
        .payload_bytes = static_cast<uint16_t>(static_cast<uint16_t>(frame_.bytes[6]) |
                                               (static_cast<uint16_t>(frame_.bytes[7]) << 8U)),
    };
    if (expected_size_ != kSerialVoiceHeaderSize) return FinishFrame();
    if (!detail::IsValidSerialVoiceHeader(header) || frame_.size + header.payload_bytes > frame_.bytes.size()) {
        Reset();
        return std::nullopt;
    }
    expected_size_ += header.payload_bytes;
    return expected_size_ == frame_.size ? FinishFrame() : std::nullopt;
}

Status StartUsbSerialFrameRouter() {
#ifndef ESP_PLATFORM
    return Status::Error(ErrorCode::kUnavailable, "USB 串口帧路由仅支持 ESP-IDF 目标");
#elif !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return Status::Error(ErrorCode::kUnavailable, "当前 profile 未启用 USB-Serial/JTAG console");
#else
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return Status::Ok();
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 2048,
        };
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
            g_started.store(false);
            return Status::Error(ErrorCode::kUnavailable, "初始化 USB 串口帧路由失败");
        }
    }
    g_im_queue = xQueueCreate(kImQueueDepth, sizeof(UsbSerialFrame));
    g_voice_queue = xQueueCreate(kVoiceQueueDepth, sizeof(UsbSerialFrame));
    if (g_im_queue == nullptr || g_voice_queue == nullptr) {
        if (g_im_queue != nullptr) vQueueDelete(g_im_queue);
        if (g_voice_queue != nullptr) vQueueDelete(g_voice_queue);
        g_im_queue = nullptr;
        g_voice_queue = nullptr;
        g_started.store(false);
        return Status::Error(ErrorCode::kUnavailable, "创建 USB 串口帧队列失败");
    }
#if CONFIG_SPIRAM && (configSUPPORT_STATIC_ALLOCATION == 1)
    if (xTaskCreateWithCaps(&RouterTask, "usb_frame_router", 4096, nullptr, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
#else
    if (xTaskCreate(&RouterTask, "usb_frame_router", 4096, nullptr, 4, nullptr) != pdPASS) {
#endif
        vQueueDelete(g_im_queue);
        vQueueDelete(g_voice_queue);
        g_im_queue = nullptr;
        g_voice_queue = nullptr;
        g_started.store(false);
        return Status::Error(ErrorCode::kUnavailable, "创建 USB 串口帧路由任务失败");
    }
    ESP_LOGI(kTag, "USB_SERIAL_FRAME_ROUTER_READY=1");
    return Status::Ok();
#endif
}

bool ReceiveImUsbSerialFrame(UsbSerialFrame* destination, int timeout_ms) {
#ifdef ESP_PLATFORM
    return Receive(g_im_queue, destination, timeout_ms);
#else
    (void)destination;
    (void)timeout_ms;
    return false;
#endif
}

bool ReceiveSerialVoiceUsbFrame(UsbSerialFrame* destination, int timeout_ms) {
#ifdef ESP_PLATFORM
    return Receive(g_voice_queue, destination, timeout_ms);
#else
    (void)destination;
    (void)timeout_ms;
    return false;
#endif
}

}  // namespace voicelife::runtime
