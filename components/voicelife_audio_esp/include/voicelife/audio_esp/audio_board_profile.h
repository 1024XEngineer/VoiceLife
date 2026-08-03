#pragma once

#include <cstdint>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::audio_esp {

// Board facts stay in the adapter profile. VoiceSession only sees negotiated
// AudioFormat values and never receives GPIO or codec addresses.
struct I2sPinProfile {
    int mclk = -1;
    int ws = -1;
    int bclk = -1;
    int din = -1;
    int dout = -1;
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

struct AudioBoardProfile {
    std::string id;
    uint8_t i2s_port = 0;
    I2sPinProfile i2s;
    I2cPinProfile i2c;
    CodecAddressProfile codec_addresses;
    voice::AudioFormat device_capture_format;
    voice::AudioFormat device_playback_format;
    uint8_t dma_desc_num = 6;
    uint16_t dma_frame_num = 240;
    bool input_reference = false;

    [[nodiscard]] Status Validate() const;
};

// Facts extracted from voicelife-pcb-native-mvp's lckfb/szpi-esp32s3 board
// source. The returned profile is a migration input, not proof of new-firmware
// hardware support until the probe and codec smoke have passed.
[[nodiscard]] AudioBoardProfile LichuangEsp32s3Profile();

}  // namespace voicelife::audio_esp
