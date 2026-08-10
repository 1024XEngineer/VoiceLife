# 本地唤醒词接入变更记录

结论：当前实板固件启动后直接进入 Linx realtime 采集，没有本地待机或唤醒入口；这不符合“你好牛牛”为板载唤醒词的产品要求。首版采用 ESP-SR MultiNet7 在板上识别 `ni hao niu niu`，识别成功后向 Linx 发送 `listen.detect` 并开启上行采集。

下一步动作：实现独立的 ESP 检测器和输入门控，先完成模型装载、待机状态切换和 Linx 控制面契约，再在最后一项进行真实人声验收。

## 为什么不直接复制 MVP

`voicelife-pcb-native-mvp` 的 `voicelife-pcb` 构建已启用 MultiNet7，并将 `ni hao niu niu` 映射为“你好牛牛”。它的 AFE、音频队列、设备状态、协议和 UI 都在同一个应用流程中。当前工程已经把 PCM、会话、Linx 协议和 ESP WebSocket 分开；整段搬运会重新引入第二套状态机和重复 I2S 所有权。

MVP 只提供以下可复用事实：MultiNet7 可动态注册中文命令；它的输入为 16 kHz、16-bit、单声道 PCM；识别结果可携带命令 ID；Linx 支持设备发送 `listen` / `detect`。代码只复用 ESP-SR 的公开 API 和经验证的词串，不复制 MVP 的 `Application` 或 `AudioService`。

## 本次决定

```text
I2S PCM 输入
  -> WakeGateAudioInput
       -> idle: MultiNet7（仅本地检测）
       -> detected: Linx listen.detect -> VoiceSession BeginCapture
       -> capturing: VoiceSession -> Linx 上行 PCM
       -> TTS: 停止上行采集，返回 idle 本地检测
```

- 新增 `voicelife_wake_esp`，只持有 ESP-SR 模型、命令词和检测任务；ESP-SR 类型不进入 `voicelife_voice`、`voicelife_linx` 或 Runtime 公共接口。
- `WakeGateAudioInput` 复用现有 PCM I2S 端口。在 idle 时保留 16 kHz 本地检测，只有在会话采集状态才把帧转给 `VoiceSession`。它不复制 I2S 通道，也不让网络发送阻塞 I2S 捕获任务。
- 首版固定启用“你好牛牛”。MultiNet7 理论上支持多命令，`牛牛`、`小牛`将作为后续 Profile 配置候选；两个两音节词的误唤醒风险必须由真实人声和环境噪声数据确认，当前不默认启用。
- 当前板没有 playback reference，且 TTS 开始时已经停止采集。因此“播放中以语音打断”不属于首版承诺。先保证 TTS 结束后回到本地待机；播放中打断需要单独验证扬声器回声不会触发模型。

## 资源和分区

当前板实测为 ESP32-S3、16 MB Flash、8 MB PSRAM。现有生产分区在 `voicelife` 数据分区后仍有未分配 Flash 空间；MultiNet7 中文模型和 FST 的原始文件合计约 2.67 MB。模型必须放入单独的只读 `model` 分区，不能写进 OTA app、普通 NVS 或业务数据分区。

运行时须显式启用并验证 PSRAM。若模型分区、模型校验或 PSRAM 分配失败，Runtime 必须保持不可采集的失败状态并给出脱敏错误，不得静默退化成一直向 Linx 上传麦克风音频。

## 验收边界

先完成以下非人声证据：

1. ESP-IDF 6.0.2 构建包含模型分区，镜像与模型都小于实测分区。
2. 启动日志仅记录检测器 ready、待机/采集状态和资源指标；不记录原始 PCM、识别文本、Wi-Fi、token 或设备标识。
3. 主机契约证明 idle 帧不进入 Provider，检测事件的顺序为 `detect` 再 `listen.start`，采集停止后恢复待机。
4. 真机在非活动 `ota_1` 启动，证明模型装载、PCM 帧输入、WSS 保活和 TTS 后回待机；完成后按既有流程恢复基线镜像。

最后才进行真实人声：以“你好牛牛”各完成 3 次创建日程与查询日程，记录脱敏后的唤醒、STT、MCP、TTS、帧计数、延迟和最低空闲堆。多词和本地语音打断只有在这项通过后才进入独立变更。
