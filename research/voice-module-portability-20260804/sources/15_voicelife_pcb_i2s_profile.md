# Source 15：voicelife-pcb 纯 I2S Profile 与实板对照

- 本地旧 MVP：`/Users/mac/Desktop/project/voicelife-pcb-native-mvp/firmware/main/audio/codecs/no_audio_codec.cc`
- 本地旧 MVP 板配置：`/Users/mac/Desktop/project/voicelife-pcb-native-mvp/firmware/main/boards/bread-compact-wifi/config.h`
- 当前实现：`components/voicelife_audio_esp/src/audio_board_profile.cc`
- 读取/测试日期：2026-08-04
- 类型：本地源码、ESP-IDF 构建与实板证据

## 旧 MVP 的可迁移事实

- 纯 simplex 使用 I2S0 TX + I2S1 RX；播放为 `BCLK=15 / LRCK=16 / DOUT=7`，采集为 `SCK=5 / WS=4 / DIN=6`。
- 输入 16 kHz、输出 24 kHz；两端使用 32-bit slot，旧实现将采集值右移 12 bit。
- 旧实现把任务边界、DMA 和 PCM 缩放放在 Codec 适配器中，没有把这些 GPIO 宏传播到 Domain。

## 新工程的迁移决定

`AudioBoardProfile` 将 capture/playback 端点、controller、wire slot、PCM 对齐和 Codec 控制面分开。`kDirectI2sSimplex` 不再把“必须使用不同 controller”写成通用规则，因为 ESP32-S3 官方允许同一 controller 的独立 TX/RX 通道使用不同 GPIO/时钟；当前板仍按实测使用 I2S1 + I2S0。旧 MVP 的 `>>12` 作为基线保留在研究记录中，不直接等于新 Profile 的最终增益。

## 实板对照

测试固件只写入非活动 `ota_1@0x410000`，镜像回读逐字节一致；两轮都在 115200 下采集 300 ms（4800 个样本），结束后恢复原 `otadata`，原固件从 `ota_0` 启动且 SQLite 仍加载 7 个事件、8 个提醒、0 条笔记。

| 采集右移 | 非零 | 变化 | 削波 | 削波比例 | 均方值 | 最低空闲堆 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 12（旧 MVP） | 4800 | 4400 | 383 | 79791 ppm | 206790038 | 369528 B |
| 14（当前 Profile） | 4800 | 4716 | 1 | 208 ppm | 45611365 | 369528 B |

结论：当前板采用右移 14，削波显著下降且样本变化保持，给后续 AFE/Codec 增益控制留出余量。此证据只证明数字 PCM 输入与总线级有限回放；没有声学观察，不能宣称扬声器播放或语音云端闭环已通过。
