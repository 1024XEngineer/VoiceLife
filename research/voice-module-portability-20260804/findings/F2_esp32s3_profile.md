# F2：ESP32-S3 Profile 的最低承诺

## 判断

ESP32-S3 是唯一进入当前实现和实板验证的目标。Profile 先承诺 16 kHz 单声道上行、服务端协商的独立下行格式、Opus/PCM 策略、WSS hello 生命周期和有界队列；AEC/Wake/VAD 只有在对应硬件与资源证据齐全时才打开。

## 必须记录

`chip/revision`、Flash/PSRAM、I2S RX/TX、slot/channel map、Codec/PA、playback reference、AFE 模式、Opus frame duration、队列容量、最低空闲堆、掉帧/欠载、固件 commit 和实板日志。

## 不做的承诺

没有 reference 的板不能写“全双工 AEC”；没有真实 WSS/ASR/TTS 日志不能写“云端闭环完成”；只有主机测试不能代替板上录放和物理闭环。
