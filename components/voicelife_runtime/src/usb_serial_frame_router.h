#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {

/// USB-Serial/JTAG messages that may share the runtime console.
enum class UsbSerialFrameKind : uint8_t {
    kImProvisioning,
    kImPairing,
    kSerialVoice,
};

/// The largest valid VLI1/VLI2 frame: header + origin + device ID + token + user ID.
inline constexpr std::size_t kUsbSerialMaximumFrameBytes = 12U + 255U + 128U + 512U + 128U;

struct UsbSerialFrame {
    UsbSerialFrameKind kind = UsbSerialFrameKind::kImProvisioning;
    std::size_t size = 0;
    std::array<uint8_t, kUsbSerialMaximumFrameBytes> bytes{};

    [[nodiscard]] std::span<const uint8_t> view() const { return {bytes.data(), size}; }
};

/**
 * Incrementally parses the three runtime USB protocols. A complete frame is
 * emitted only after its own header and bounded payload have been validated.
 */
class UsbSerialFrameDecoder final {
   public:
    [[nodiscard]] std::optional<UsbSerialFrame> Push(uint8_t byte);

   private:
    enum class State : uint8_t {
        kSearchingMagic,
        kReadingFrame,
    };

    void Reset();
    [[nodiscard]] bool IsMagicPrefix() const;
    [[nodiscard]] std::optional<UsbSerialFrame> FinishFrame();

    State state_ = State::kSearchingMagic;
    UsbSerialFrame frame_{};
    std::size_t expected_size_ = 0;
};

/** Starts the sole USB-Serial/JTAG reader and its bounded per-protocol queues. */
Status StartUsbSerialFrameRouter();

/** Receives a VLI1/VLI2 or VLP1 frame without touching the physical USB RX queue. */
bool ReceiveImUsbSerialFrame(UsbSerialFrame* destination, int timeout_ms);

/** Receives a VLVT-v1 frame without touching the physical USB RX queue. */
bool ReceiveSerialVoiceUsbFrame(UsbSerialFrame* destination, int timeout_ms);

}  // namespace voicelife::runtime
