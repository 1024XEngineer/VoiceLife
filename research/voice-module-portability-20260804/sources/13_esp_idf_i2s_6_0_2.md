# Source 13：ESP-IDF 6.0.2 ESP32-S3 I2S 当前边界

- URL：<https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/docs/en/api-reference/peripherals/i2s.rst>
- 读取日期：2026-08-04
- 类型：Espressif 官方版本化文档

## 原文摘录

> `ESP32-S3` contains two I2S peripheral(s).

> `ESP32-S3` supports standard, PDM and TDM modes on I2S 0/1; PDM-to-PCM conversion is target/port dependent.

> Each controller has separate RX and TX channels. They are able to work under different clocks and slot configurations with separate GPIO pins.

> `i2s_channel_read` and `i2s_channel_write` are blocking functions; the driver also exposes an interrupt callback, where complex or non-reentrant work must not run.

> With power management enabled, the driver acquires a clock lock while reading or writing so light sleep cannot invalidate the I2S clock during the transfer.

## 对本项目的约束

1. ESP32-S3 Profile 可以把采集与播放描述成独立的 RX/TX 配置，但不能只填一个“全双工采样率”。`VoiceAudioFormats.capture` 与 `.playback` 仍然是上层稳定契约。
2. I2S callback 只做固定大小的 DMA 交接或投递到有界队列；不得在中断上下文中分配 `std::vector`、访问 SQLite、调用网络或执行 AFE 控制。
3. 使用 PDM 麦克风前必须按实际 I2S controller 验证 PDM-to-PCM 转换；没有硬件转换器时，Profile 必须显式声明软件滤波和资源预算，不能把原始 PDM 当 PCM 交给 Provider。
4. Adapter 的 Start/Stop/Close 必须遵守 `registered -> ready -> running` 生命周期，重配置只能在停止通道后执行。
5. 实机测试需记录省电配置、I2S 时钟源、DMA 帧数、超时和最低空闲堆；“I2S 初始化成功”不是录放可用证据。

## 限制

官方 I2S 文档不包含当前立创实战派板的 Codec 型号、PCA9557 音频电源控制或 GPIO 连线；这些事实来自旧 MVP 的板级源码，仍需在当前工程完成 codec 初始化和真实录放测试后才能进入已支持 Profile。
