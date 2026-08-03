# Source 04：ESP-IDF I2S

- URL：<https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html>
- 读取日期：2026-08-04
- 类型：Espressif 官方文档

## 原文摘录

> ESP32-S3 contains two I2S peripheral(s).

> Each controller has separate RX and TX channels. That means they are able to work under different clocks and slot configurations with separate GPIO pins.

> `i2s_std` demonstrates how to use the I2S standard mode in either simplex or full-duplex mode on ESP32-S3.

## 对本项目的约束

ESP32-S3 可以让 capture 与 playback 拥有不同采样率和 GPIO，但“外设支持全双工”不等于“当前板有 playback reference 或已通过 AEC”。板级 Profile 必须记录 RX/TX、slot、DMA 和 reference 能力。
