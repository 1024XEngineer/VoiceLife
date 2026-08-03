# Source 05：ESP-SR AFE

- URL：<https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html>
- 读取日期：2026-07-29（本轮复核本地来源）
- 类型：Espressif 官方文档

## 原文摘录

> It includes various algorithms like Acoustic Echo Cancellation (AEC), Noise Suppression (NS), Voice Activity Detection (VAD), and Wake Word Detection (WakeNet).

> `MMNR` Indicates four channels inorder: microphone channel, microphone channel, unused channel, and playback reference channel.

> The input data must be arranged in channel-interleaved format.

> Channel-interleaved audio data (16-bit signed, 16 kHz).

## 对本项目的判断

AFE 是音频前端软件，不是麦克风或功放。没有真实播放 reference 时，ESP32-S3 Profile 只能声明 AEC-0（半双工/auto-stop），不能因为启用了 AFE 就宣称全双工 AEC。
