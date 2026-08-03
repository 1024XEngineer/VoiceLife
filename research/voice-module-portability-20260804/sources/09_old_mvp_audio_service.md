# Source 09：voicelife-pcb-native-mvp AudioService

- 本地文件：`/Users/mac/Desktop/project/voicelife-pcb-native-mvp/firmware/main/audio/audio_service.cc`
- 读取日期：2026-08-04
- 类型：本地实现与实板证据

## 关键行

- `AudioInputTask` 每次读取 10 ms、16 kHz PCM，并将数据送入唯一 `AudioEngine`。
- `OpusCodecTask` 独立完成编码/解码，`AudioOutputTask` 独立写 Codec。
- 上行编码队列和发送队列满载时丢最旧帧；网络不反压输入任务。
- 播放解码以 `playback_generation_` 绑定，`ResetDecoder` 清空 decode/playback/timestamp 队列。
- 断电/读超时不会终止输入任务，输入硬件由启动它的任务关闭。

## 新架构保留与删除

保留任务边界、有界队列、generation 和固定采样率协商；删除全局 Application 状态、板级宏和直接耦合 Protocol 的 Service。新公共队列契约见 `components/voicelife_voice/.../audio_frame_queue.h`。
