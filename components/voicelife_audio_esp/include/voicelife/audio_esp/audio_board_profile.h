#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::audio_esp {

// Board facts stay in the adapter profile. VoiceSession only sees negotiated
// AudioFormat values and never receives GPIO or codec addresses.
enum class AudioBoardTopology : uint8_t {
    kExternalCodecDuplex,
    kDirectI2sSimplex,
};

struct I2sEndpointProfile {
    uint8_t port = 0;
    int mclk = -1;
    int bclk = -1;
    int ws = -1;
    int data = -1;
    voice::AudioFormat format;
    uint8_t wire_bits_per_sample = 16;
    // Direct-I2S microphones commonly expose a wider wire sample than the
    // logical PCM16 frame. Capture shifts right; playback shifts left.
    uint8_t pcm_shift_bits = 0;
};

struct I2cPinProfile {
    int sda = -1;
    int scl = -1;
};

struct CodecAddressProfile {
    // esp_codec_dev expects codec addresses with the read/write bit included.
    uint8_t es8311_8bit = 0;
    uint8_t es7210_8bit = 0;
    // PCA9557 is accessed directly through the ESP-IDF I2C master API.
    uint8_t pca9557_7bit = 0;
};

struct CodecControlProfile {
    uint8_t i2c_port = 1;
    I2cPinProfile i2c;
    CodecAddressProfile addresses;
};

struct AudioBoardProfile {
    std::string id;
    AudioBoardTopology topology = AudioBoardTopology::kExternalCodecDuplex;
    I2sEndpointProfile capture_i2s;
    I2sEndpointProfile playback_i2s;
    std::optional<CodecControlProfile> codec_control;
    uint8_t dma_desc_num = 6;
    uint16_t dma_frame_num = 240;
    bool input_reference = false;

    [[nodiscard]] Status Validate() const;
};

// Facts extracted from voicelife-pcb-native-mvp's lckfb/szpi-esp32s3 board
// source. The returned profile is a migration input, not proof of new-firmware
// hardware support until the probe and codec smoke have passed.
[[nodiscard]] AudioBoardProfile LichuangEsp32s3Profile();

// Facts verified against the currently connected SKU=voicelife-pcb board and
// its original NoAudioCodecSimplex implementation. Runtime support still
// requires the dedicated physical probe to pass.
[[nodiscard]] AudioBoardProfile VoiceLifePcbEsp32s3Profile();

}  // namespace voicelife::audio_esp
