#pragma once

#include <array>
#include <mutex>

#include "voicelife/audio_esp/esp32s3_pcm_audio_port.h"
#include "voicelife/audio_esp/esp_multinet_wake_detector.h"
#include "voicelife/audio_esp/esp_wakenet_detector.h"
#include "voicelife/board_esp/gpio46_power_arbiter.h"
#include "voicelife/board_esp/sparkbot_wake_model_assets.h"
#include "voicelife/display_esp/ssd1306_presentation_adapter.h"
#include "voicelife/display_sparkbot/sparkbot_presentation_adapter.h"
#include "voicelife/runtime/platform_assembly.h"
#include "voicelife/voice/wake_gate_audio_input.h"

namespace voicelife::runtime {

/**
 * @brief VoiceLife PCB 平台装配：Ssd1306PresentationAdapter。
 *
 * 旧 VoiceLife PCB 继续走点阵 SSD1306 显示路径；本包装是迁移起点，
 * 不是冻结旧板的永久实现，后续可独立演进。
 */
class VoiceLifePcbAssembly : public PlatformAssembly {
   public:
    /** @brief 构造函数：按 PCB 音频 Profile 装配 PCM 端口。 */
    VoiceLifePcbAssembly();

    /** @brief 虚析构函数。 */
    ~VoiceLifePcbAssembly() override = default;

    /** @brief 返回点阵显示端口。 @return Ssd1306PresentationAdapter。 */
    voicelife::voice::PresentationPort& presentation() override;

    /** @brief 启动 PCB 物理输入到语义事件的映射。 */
    voicelife::Status StartBoardInput(BoardInputSink sink) override;

    /** @brief 返回板级音频采集端口。 @return PCM 输入端口。 */
    voicelife::voice::AudioInputPort& audio_input() override;
    /** @brief 返回板级音频播放端口。 @return PCM 输出端口。 */
    voicelife::voice::AudioOutputPort& audio_output() override;
    /** @brief 设置输出音量。 @param volume 音量 0-100。 */
    void SetOutputVolume(uint8_t volume) override;
    /** @brief 打印 PCM 音频统计。 */
    void LogAudioStats() override;
    /** @brief 返回 PCB 唤醒门控。 @return WakeGateAudioInput。 */
    voicelife::voice::WakeGateAudioInput& wake_gate() override;
    /** @brief 返回 PCB 板型身份。 @return voicelife-pcb。 */
    std::string_view board_identity() const override { return "voicelife-pcb"; }
    /** @brief 初始化 PCB LED（GPIO48 锁定灭）。 */
    void InitializeBoardLeds() override;

   private:
    struct ButtonSample {
        int gpio = -1;
        bool previous_pressed = false;
        bool long_fired = false;
        int64_t pressed_at_us = 0;
    };

    static void BoardInputTaskEntry(void* context);
    void BoardInputTask();
    voicelife::Status StartGpioInput(std::array<int, 4> gpios, BoardInputSink sink, const char* task_name);

    [[maybe_unused]] std::array<ButtonSample, 4> buttons_{};
    [[maybe_unused]] std::size_t button_count_ = 0;
    BoardInputSink board_input_sink_;
    std::unique_ptr<voicelife::audio_esp::EspMultiNetWakeDetector> wake_detector_;
    std::unique_ptr<voicelife::voice::WakeGateAudioInput> wake_gate_;
    voicelife::audio_esp::Esp32s3PcmAudioPorts audio_ports_;
    voicelife::display_esp::Ssd1306PresentationAdapter ssd1306_adapter_;
};

/**
 * @brief ESP-SparkBot 平台装配：完整 SparkBotPresentationAdapter。
 *
 * 调用链：presentation() -> 有界队列 -> 专属显示任务 -> 官方 Renderer；
 * Start() 初始化 ST7789/LVGL 并启动显示任务。实板显示未验证，available
 * 为 true 仅表示显示链路（代码级）闭合。
 */
class SparkBotAssembly : public PlatformAssembly {
   public:
    /** @brief 构造函数：按官方板级 Profile 准备显示配置。 */
    SparkBotAssembly();

    /** @brief 虚析构函数。 */
    ~SparkBotAssembly() override = default;

    /** @brief 返回 SparkBot 彩屏显示端口。 @return SparkBotPresentationAdapter。 */
    voicelife::voice::PresentationPort& presentation() override;

    /** @brief 初始化 ST7789/LVGL 并启动专属显示任务。 @return 启动结果。 */
    voicelife::Status Start() override;

    /** @brief 启动 SparkBot BOOT 键到语义事件的映射。 */
    voicelife::Status StartBoardInput(BoardInputSink sink) override;

    /** @brief 音频功放请求（经统一仲裁）。 @param enabled 是否启用功放。 @return 仲裁结果。 */
    voicelife::Status SetAudioOutputEnabled(bool enabled) override;

    /** @brief 返回板级音频采集端口。 @return ES8311 输入端口。 */
    voicelife::voice::AudioInputPort& audio_input() override;
    /** @brief 返回板级音频播放端口。 @return ES8311 输出端口。 */
    voicelife::voice::AudioOutputPort& audio_output() override;
    /** @brief 设置输出音量。 @param volume 音量 0-100。 */
    void SetOutputVolume(uint8_t volume) override;
    /** @brief 打印 ES8311 音频统计。 */
    void LogAudioStats() override;
    /** @brief 返回 SparkBot 唤醒门控。 @return WakeGateAudioInput。 */
    voicelife::voice::WakeGateAudioInput& wake_gate() override;
    /** @brief 返回 SparkBot 板型身份。 @return esp-sparkbot。 */
    std::string_view board_identity() const override { return "esp-sparkbot"; }
    /** @brief SparkBot 使用 assets 中受控 WakeNet 模型待机监听。 */
    bool uses_local_wake_detector() const override { return wake_ready_; }
    /** @brief SparkBot 无 LED（GPIO48 为底盘 UART RX，不写入）。 */
    void InitializeBoardLeds() override {}

   private:
    struct ButtonSample {
        int gpio = -1;
        bool previous_pressed = false;
        bool long_fired = false;
        int64_t pressed_at_us = 0;
    };

    static void BoardInputTaskEntry(void* context);
    void BoardInputTask();

    /** @brief 经板级仲裁更新 GPIO46 背光（ESP 构建写 GPIO）。 */
    void ApplyBacklight(bool enabled);

    /** @brief 配置 GPIO46 为输出（唯一物理 owner，ESP 构建）。 */
    void ConfigureSharedPowerGpio();

    /** @brief 按仲裁结果写 GPIO46 电平（ESP 构建；调用方必须已持锁）。 */
    void WriteSharedPowerLineLocked();

    /** @brief GPIO46 仲裁与写入互斥（显示回调与音频任务并发保护）。 */
    mutable std::mutex power_mutex_;
    ButtonSample boot_button_{};
    BoardInputSink board_input_sink_;
    voicelife::board_esp::SparkBotWakeModelAssets wake_model_assets_;
    std::unique_ptr<voicelife::audio_esp::EspWakeNetDetector> wake_detector_;
    std::unique_ptr<voicelife::voice::WakeGateAudioInput> wake_gate_;
    bool wake_ready_ = false;
    voicelife::audio_esp::Esp32s3PcmAudioPorts audio_ports_;
    voicelife::board_esp::Gpio46PowerArbiter arbiter_;
    voicelife::display_sparkbot::SparkBotPresentationAdapter adapter_;
};

}  // namespace voicelife::runtime
