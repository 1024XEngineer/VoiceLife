# Source 02：xiaozhi-esp32 AudioService

- URL：<https://raw.githubusercontent.com/78/xiaozhi-esp32/main/main/audio/audio_service.h>
- 读取日期：2026-08-04
- 类型：固定上游实现入口

## 原文摘录

> `(MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)`

> `(Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)`

> `We use dedicated tasks for input, output, and Opus encoding/decoding.`

上游同时声明了固定容量：编码任务队列为 2，播放任务队列为 2，发送/解码队列按约 2400 ms 的音频预算计算，Opus 首个配置为 16 kHz、单声道、16 bit、60 ms。

## 对本项目的迁移判断

这组分任务和容量是实时性经验，不是 Voice Domain 的业务接口。新架构把它表达为 `AudioInputPort`、`AudioOutputPort`、`CodecStrategy` 和 `BoundedAudioFrameQueue`，队列满载策略与水位计数必须可见。

## 限制

小智的 `AudioService` 依赖 ESP-IDF/FreeRTOS 和板级 Codec，不能直接成为跨板卡核心；其参数需要在新 Profile 上重新测量。
