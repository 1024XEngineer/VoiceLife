# Source 14：ESP32 系列跨板音频能力矩阵

- 主要来源：<https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/docs/en/api-reference/peripherals/i2s.rst>
- 补充来源：<https://docs.zephyrproject.org/latest/services/audio/index.html>
- 读取日期：2026-08-04
- 类型：芯片厂商文档与 RTOS 官方服务文档

## 能力矩阵

| 目标 | 官方 I2S 端口/模式摘要 | 当前项目判断 | 进入实现前必须补的证据 |
| --- | --- | --- | --- |
| ESP32-S3 | I2S 0/1；standard、PDM、TDM；I2S0 支持 PDM-to-PCM | 唯一主验证平台 | 当前板 Codec/I2C/PCA9557、实际 GPIO、DMA/堆、录放和 AEC reference |
| ESP32-C3 | I2S0；standard、PDM、PCM-to-PDM；无 PDM-to-PCM | 低资源对照，不复用 S3 AFE 声明 | 外部数字麦克风格式、软件 PDM 处理、Opus/RAM 和网络并发 |
| ESP32-C6 | I2S0；standard、PDM、PCM-to-PDM；无 PDM-to-PCM | 低资源 Wi-Fi 6 对照 | 与 C3 相同，另测 Wi-Fi 6 共存、功耗和时钟 |
| ESP32-P4 | I2S0/1/2；standard、PDM、TDM；I2S0 支持 PDM-to-PCM | 高资源后续候选 | IDF/Codec 版本、功耗、PSRAM、音频 DMA 和安全启动 |
| RP2350 / STM32 / nRF | 不由 ESP-IDF 矩阵覆盖 | 只做能力探针，不进入当前固件 | 具体 HAL/Zephyr driver、网络协处理器、存储掉电和编解码预算 |

## 设计结论

- “同一个 Port 可以被多个 Adapter 实现”是可迁移性；“一份二进制覆盖所有板”不是目标。
- 新增板卡前先完成 `capability probe -> host contract -> board smoke -> resource budget -> recovery` 五步准入。
- S3 的 AEC/Wake 能力不能向 C3/C6/P4 自动继承。每个 Profile 单独声明 `i2s-mode`、`pdm-to-pcm`、`aec-reference`、`psram`、`codec-control` 和 `network-duplex`。

## 限制

矩阵描述的是芯片外设能力，不是某块开发板已经接好麦克风/扬声器。板级 Codec、电源、引脚和声学布局仍必须单独核对和实测。
