# F1：实时边界必须先于 Provider 选择

## 判断

H1、H2 得到支持。采集、编解码、网络传输、播放和业务写入需要不同队列和所有权。`voicelife-pcb-native-mvp` 与小智源码给出实现证据，Linx 官方文档给出协议时序，SQLite 官方文档说明同步和 VFS 是另一条故障边界。

## 落地

- `AudioInputPort` / `AudioOutputPort` 只处理音频帧与硬件生命周期；输入端口只提交格式和负载，`VoiceSession` 统一补齐 generation/sequence。
- `SpeechProviderAdapter` 只处理 hello、listen、STT/TTS、abort 和能力。
- SQLite 写入由应用控制面提交；音频任务不得打开数据库、等待事务或记录原始 PCM。
- `BoundedAudioFrameQueue` 公开满载策略、generation、high-watermark 和丢帧统计。

## 反例

一个“大 AudioService”可以在单板 MVP 上工作，但会让替换 Linx、Codec 或文件系统时修改业务状态机；这正是本次重构要避免的扩散。
