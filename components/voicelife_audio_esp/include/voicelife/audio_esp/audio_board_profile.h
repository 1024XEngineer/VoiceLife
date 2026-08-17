#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/contracts/status.h"
#include "voicelife/voice/voice_types.h"

namespace voicelife::audio_esp {

/**
 * @brief 音频板拓扑类型。
 *
 * 板级事实保留在适配器 Profile 中，VoiceSession 只看到协商的
 * AudioFormat 值，绝不接收 GPIO 或 Codec 地址。
 */
enum class AudioBoardTopology : uint8_t {
    /** @brief 外部 Codec 全双工。 */
    kExternalCodecDuplex,
    /** @brief 直连 I2S 半双工。 */
    kDirectI2sSimplex,
};

/** @brief I2S 端点 Profile：引脚与音频格式。 */
struct I2sEndpointProfile {
    /** @brief I2S 端口号。 */
    uint8_t port = 0;
    /** @brief MCLK 引脚。 */
    int mclk = -1;
    /** @brief BCLK 引脚。 */
    int bclk = -1;
    /** @brief WS 引脚。 */
    int ws = -1;
    /** @brief 数据引脚。 */
    int data = -1;
    /** @brief 音频格式。 */
    voice::AudioFormat format;
    /** @brief 线上位宽（每样本比特数）。 */
    uint8_t wire_bits_per_sample = 16;
    /**
     * @brief PCM 移位比特数。
     *
     * 直连 I2S 麦克风常暴露比逻辑 PCM16 帧更宽的线上样本；
     * 采集右移、播放左移。
     */
    uint8_t pcm_shift_bits = 0;
    /**
     * @brief I2S 物理时隙数；0 表示与逻辑 PCM 通道数一致。
     *
     * VoiceSession 可保持单声道，而 ES8311 仍按板级时序收发 stereo/BOTH slots。
     */
    uint8_t wire_slot_count = 0;
};

/** @brief I2C 引脚 Profile。 */
struct I2cPinProfile {
    /** @brief SDA 引脚。 */
    int sda = -1;
    /** @brief SCL 引脚。 */
    int scl = -1;
};

/** @brief Codec 地址 Profile。 */
struct CodecAddressProfile {
    /** @brief ES8311 8 位地址（含读写位）。 */
    uint8_t es8311_8bit = 0;
    /** @brief ES7210 8 位地址（含读写位）。 */
    uint8_t es7210_8bit = 0;
    /** @brief PCA9557 7 位地址（经 ESP-IDF I2C 主控直访）。 */
    uint8_t pca9557_7bit = 0;
};

/** @brief Codec 控制 Profile：I2C 端口与引脚。 */
struct CodecControlProfile {
    /** @brief I2C 端口号。 */
    uint8_t i2c_port = 1;
    /** @brief I2C 引脚。 */
    I2cPinProfile i2c;
    /** @brief Codec 地址。 */
    CodecAddressProfile addresses;
};

/** @brief 音频板 Profile：拓扑、I2S 端点与 Codec 控制。 */
struct AudioBoardProfile {
    /** @brief Profile ID。 */
    std::string id;
    /** @brief 板级拓扑。 */
    AudioBoardTopology topology = AudioBoardTopology::kExternalCodecDuplex;
    /** @brief 采集 I2S 端点。 */
    I2sEndpointProfile capture_i2s;
    /** @brief 播放 I2S 端点。 */
    I2sEndpointProfile playback_i2s;
    /** @brief 可选 Codec 控制配置。 */
    std::optional<CodecControlProfile> codec_control;
    /** @brief DMA 描述符数量。 */
    uint8_t dma_desc_num = 6;
    /** @brief DMA 帧数量。 */
    uint16_t dma_frame_num = 240;
    /** @brief 是否启用输入参考信号。 */
    bool input_reference = false;

    /** @brief 校验 Profile 字段合法性。 @return 合法返回 Ok。 */
    [[nodiscard]] Status Validate() const;
};

/**
 * @brief 立创 ESP32-S3 板的 Profile。
 *
 * 从 voicelife-pcb-native-mvp 的 lckfb/szpi-esp32s3 板源码提取的事实。
 * 返回的 Profile 是迁移输入，在探针和 Codec smoke 通过前
 * 不构成新固件硬件支持证明。
 * @return 立创板 Profile。
 */
[[nodiscard]] AudioBoardProfile LichuangEsp32s3Profile();

/**
 * @brief 当前 voicelife-pcb 板的 Profile。
 *
 * 依据当前连接 SKU=voicelife-pcb 板及其 NoAudioCodecSimplex 原始实现
 * 核对的事实。运行时支持仍需专用物理探针通过。
 * @return voicelife-pcb 板 Profile。
 */
[[nodiscard]] AudioBoardProfile VoiceLifePcbEsp32s3Profile();

/**
 * @brief ESP-SparkBot 板的 ES8311 双工音频 Profile。
 *
 * 引脚与采样率取自官方 xiaozhi-esp32@37d1aee esp_sparkbot_board.cc /
 * config.h（I2S mclk45/bclk39/ws41/din40/dout42，I2C sda4/scl5，
 * ES8311 0x18，16kHz）。音频主链实板验证属于后续阶段；本 Profile 用于
 * 保证 Runtime 不再固定 VoiceLife PCB 引脚。
 * @return SparkBot 音频 Profile。
 */
[[nodiscard]] AudioBoardProfile SparkBotEsp32s3AudioProfile();

}  // namespace voicelife::audio_esp
