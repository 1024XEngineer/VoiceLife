# 语音模块子架构

一句话结论：VoiceLife 的语音模块采用“实时音频数据面 + 会话控制面 + Provider 防腐层”的三段式结构，ESP32-S3 是本期唯一主验证平台，Linx XRobot WebSocket 是首个真实 Provider，小智协议作为可回退迁移来源。
下一步动作：先合并 Port、状态和契约测试骨架，再实现 ESP32-S3 的 I2S/AFE、Opus/PCM、Linx WebSocket 和真机重连；其他 MCU 只进入 Profile 与能力调研，不提前宣称可用。

## 1. 为什么单独拆语音

旧 PCB MVP 已经证明了“采集 → 上行 → ASR → TTS → 播放 → 提醒”的产品路径，但它把音频任务、WebSocket 事件、工具调用和业务状态放在同一个应用流程里。这样做在一块板上能快速演示，换语音服务或处理打断时会把改动扩散到业务层。

新主干只保留稳定语义：音频帧、会话、generation、语音事件和能力。Linx 的 JSON 字段、WebSocket 句柄、ESP-IDF I2S/AEC 句柄和小智全局状态机都留在 Adapter 内。

## 2. 两个平面

```text
实时数据面（不能被 SQLite 或业务阻塞）
I2S/AFE -> AudioInput -> Codec Strategy -> bounded queue -> VoiceTransport
VoiceTransport -> Codec Strategy -> AudioOutput -> I2S/DAC

会话控制面（允许有限超时和重连）
Runtime -> Provider Factory -> VoiceSession
VoiceSession -> hello / listen / abort / capability check
VoiceSession <- stt / tts / tool-call / error events
VoiceSession -> Application / MCP（只交稳定语义）
```

小智音频服务的当前实现将采集、编码、发送与接收、解码、播放放在独立任务和有界队列中；本项目沿用这个事实边界，但不复制它的全局状态和板卡目录。旧 MVP 的 `AudioService` 具体给出三条可迁移的工程约束：

- `AudioInputTask` 只读取 Codec 并投递 PCM；`OpusCodecTask` 独立处理编码/解码；`AudioOutputTask` 只负责把播放队列写回 Codec；
- 编码、发送、解码、播放队列都是固定上限，队列满时按策略丢弃或等待，不能让网络或业务反向阻塞麦克风；
- AFE 的 WakeNet/MultiNet、VAD 和 AEC 共用一个处理实例，并用控制代次拒绝 disable/re-enable 期间迟到的 fetch 结果。

新实现将这些事实分别落到 `AudioInputPort`、`AudioOutputPort`、`CodecStrategy`、有界队列适配器和 `generation` 契约中；不会把旧 MVP 的 FreeRTOS 全局事件位、板卡宏或 `Application` 状态机带入核心。SQLite 提交必须进入业务队列，不能从音频任务直接持有数据库连接。当前实板提交中位数约 1.16 秒，已经超过实时音频路径预算。

## 3. 代码契约

### 3.1 稳定值对象

- `AudioFormat`：编码、采样率、声道、位深和帧时长；当前 ESP32-S3 首选单声道 16 kHz、16 bit，Provider 可在握手后协商实际格式。
- `AudioFrame`：`generation + sequence + format + payload`。generation 用于隔离重连/打断前的迟到帧，sequence 用于发现丢帧和乱序。
- `VoiceSessionConfig`：Provider ID、会话模式、音频偏好、握手超时、重连退避和 MCP 能力开关；不保存 token。
- `CapabilityProfile`：Provider 对外承诺的能力，例如 `streaming-asr`、`tts`、`cancel-generation`、`mcp`、`aec`。
- `VoiceEvidence`：会话 ID、generation、事件和细节。证据只记录状态与标识，不记录 token、原始隐私文本或完整音频。

### 3.2 Port 与实现

| Port | 责任 | ESP32-S3 首个实现 | 其他实现策略 |
| --- | --- | --- | --- |
| `AudioInputPort` | 打开输入、开始/停止采集、关闭 | ESP-IDF I2S + AFE/Wake Adapter | Zephyr I2S、厂商 HAL，先做能力探针 |
| `AudioOutputPort` | 接收解码帧、刷新缓冲、关闭 | ESP-IDF I2S/DAC Adapter | 各板 Codec/扬声器驱动 |
| `VoiceTransportPort` | 连接、文本帧、二进制音频帧、关闭 | TLS WebSocket Adapter | MQTT/UDP 仅在契约满足时接入 |
| `CodecStrategy` | PCM/Opus 编解码 | 小智 Opus 参数迁移 | PCM 直通或其他硬件 Codec |
| `SpeechProviderAdapter` | Provider 生命周期、采集、播报、打断、能力 | `voicelife_linx` 的 `LinxSpeechProviderAdapter` | `xiaozhi-websocket`、主机 fake |
| `ASRAdapter` / `TTSAdapter` / `RealtimeAdapter` | 外部事件映射与模式差异 | 由 Provider 组合 | 不强迫所有 Provider 继承万能基类 |
| `EvidenceSink` | 记录可关联事件 | 串口/JSON 证据 | CI artifact、真机日志和 JUnit 摘要 |

### 3.3 会话状态与安全迁移

```text
STOPPED -> STARTING -> READY -> CAPTURING -> READY
                         READY -> SPEAKING -> READY
                         CAPTURING/SPEAKING -> READY (INTERRUPT)
任何启动失败 -> FAILED；Stop 始终回收输入、输出和 Provider
```

状态规则：

1. `Start` 先校验配置，再依次打开输入、输出和 Provider；后一步失败必须回滚前一步资源。
2. `BeginCapture` 同时通知 Provider 和本地输入；输入失败时发送停止，不能留下半开的远端 listen epoch。
3. `SubmitAudio` 只接受当前 generation 且严格连续的 sequence；旧 generation 直接丢弃并记录证据。
4. `Interrupt` 发送 Provider abort、刷新输出队列，然后递增 generation；本地不依赖云端迟到的 `tts.stop` 才恢复可用。
5. `Stop` 幂等，关闭顺序固定为 Provider → Output → Input，并使旧 generation 全部失效。

## 4. Linx XRobot WebSocket 防腐层

官方协议（2026-08-04 读取）给出的接入边界如下。协议编解码和 Provider 行为落在 `components/voicelife_linx`，ESP-IDF 的 socket 句柄、TLS、token 解析落在 `components/voicelife_linx_esp`；两层之间只通过 `LinxTransportPort` 协作。Transport 已能进入 ESP-IDF 6.0.2 / ESP32-S3 固件构建，但还没有被 Runtime 创建，也没有被真实板刷写验证。

- 推荐 `wss://xrobo-io.qiniuapi.com/v1/ws/`，内网可用 `ws://xrobo-io.qiniuapi.com/v1/ws/`；地址也可由 OTA 动态下发。
- 握手头包含 `Authorization: Bearer <token>`、`Protocol-Version: 1`、`Device-Id` 和 `Client-Id`。token 只能来自 `secret://`/NVS/安全配置引用。
- 连接后设备发送 `hello`，声明 `transport=websocket`、MCP 能力和 `audio_params`；服务端返回 hello 后才进入会话。
- 音频二进制帧支持 OPUS 与 PCM。首选 ESP32-S3 配置为单声道 16 kHz；OPUS 默认 60 ms 帧，PCM 示例为 20 ms、16 bit、小端序。首个 Adapter 会校验服务端 hello 的参数，转码和重协商留给后续 Codec Strategy。
- `listen(start|stop|detect)`、`stt`、`tts(start|sentence_start|stop)`、`abort` 和 MCP 工具消息均先在 Adapter 映射为 `VoiceEvent` 或 `ToolCall`。
- hello 超时默认 10 秒；异常断线关闭音频发送通道，按 Profile 的退避重连。当前首个 Adapter 要求服务端 hello 的音频参数与请求完全一致；真正的转码/重协商必须另建 Codec Strategy 和契约测试。重连成功前不复用旧 connection/generation。

协议字段不进入 `VoiceSession`。Provider 负责版本、字段类型、最大消息长度、session 绑定和错误码映射；核心只看到 `Status`、`AudioFrame` 和 `VoiceEvent`。

### 4.1 已锁定的离线契约

`LinxJsonCodec` 只负责文档化的控制面消息：

- `hello`：编码版本、WebSocket transport 和音频参数；token 只以 `secret://` 等引用传给 Transport；
- `listen/start|stop|detect`：手动/自动/实时模式和可选 `session_id`；
- `abort`：必须带非空原因，供打断与服务端回放取消关联；
- `hello`、`stt`、`tts/start|sentence_start|stop`、`error`：解析为稳定事件；未知类型、未知 TTS 状态、类型错误和非法音频参数直接失败；
- 二进制帧：由 `LinxSpeechProviderAdapter` 绑定当前连接 generation、单调 sequence 和协商后的 `AudioFormat`，再交给会话的 `AudioFrameSink`。

这些行为由主机 fake Transport 测试，不依赖外网。真正的 ESP32-S3 Transport 必须复用同一套编解码契约，并额外提供 header、hello 超时、断线和资源预算证据。

来源：

- [Linx WebSocket 协议](https://linx.qiniu.com/docs/xrobot/platform/websocket)
- [Linx 开源文档仓库](https://github.com/qiniu/Xrobot-docs/blob/main/docs/xrobot/platform/websocket.md)
- [Linx 小智固件接入指南](https://linx.qiniu.com/docs/xrobot/guide/xiaozhi-firmware)

### 4.2 ESP-IDF Transport 当前实现

`components/voicelife_linx_esp` 是平台适配层，不向上暴露 ESP-IDF 头文件：

- `EspWebSocketTransport` 用 PImpl 隔离 `esp_websocket_client`，依赖固定为 `espressif/esp_websocket_client==1.8.0`；WSS 使用证书 bundle，明确关闭 `skip_cert_common_name_check`。
- `SecretResolverPort` 只接收 `secret://` 等引用并返回受控 token；Transport 组装 `Authorization`、`Protocol-Version`、`Device-Id`、`Client-Id`，日志不打印 header 内容。
- ESP WebSocket callback 只复制 `data_ptr/data_len/payload_len/payload_offset/fin/op_code` 到固定大小的 FreeRTOS 队列。队列满时丢弃事件并把 Transport 标成失败，不在 callback 内调用 stop/destroy。
- `WebSocketFragmentAssembler` 在主机和 ESP32-S3 共用，处理 text/binary/continuation、非法 offset、消息大小上限、连接关闭清理和 generation 隔离；完整消息才交给 `LinxTransportSink`。
- Provider 在发送 hello 后等待服务端 hello；超时、音频参数不一致或 Transport error 都以 `Status`/`VoiceEvent` 返回，不把半连接状态交给 `VoiceSession`。

这层现在是“可构建、可单测、未上板”的状态。Issue #107 的真机顺序是：固定 115200 备份与设备确认 → 受控 WSS/echo → Linx hello → 断线/重连 → 之后才迁移 I2S/AFE/Opus。不要用当前 scaffold 固件的启动日志替代真实 Provider 验收。

## 5. 小智迁移边界

小智不是新的 Domain。迁移按以下顺序推进：

1. 先把 `AudioService` 的队列、Opus 参数、AEC/Wake 资源约束包进 `AudioInput/AudioOutput/CodecStrategy`；优先迁移 `audio_service.h/.cc`、`audio_engine.h`、S3 的 `afe_audio_engine.*` 和实际使用的 `audio_codec.*`，而不是整个 `main/`；
2. 再把 `WebsocketProtocol::OpenAudioChannel` 的握手头、hello 等待、服务端音频参数解析和二进制帧边界转换为 `VoiceTransportPort`/`SpeechProviderAdapter`，保留连接 generation 和有界队列；
3. 将 `AudioService::ResetDecoder`、播放代次和打断后的队列清理映射为 `AudioOutputPort::Flush` + 会话 generation 失效，不把旧的全局 event group 传播到 Domain；
4. 最后把 MCP 工具描述和调用映射到现有 `ToolGatewayPort`，不把小智的工具注册中心复制进 Application。

必须固定上游 commit 和 MIT 许可；每迁移一段，都用相同音频输入、同一帧序列和同一错误注入做对照测试。当前参考基线为 `78/xiaozhi-esp32@dd99da00dc4c89ed4ab07fcec038c03f13f4de50`，实际迁移入口见旧 MVP 的 `voicelife-pcb-native-mvp/firmware/main/audio/` 与 `.../protocols/websocket_protocol.*`。来源：[78/xiaozhi-esp32 音频服务](https://github.com/78/xiaozhi-esp32/blob/main/main/audio/audio_service.h)。

## 6. 模式选择与不做过度抽象

| 模式 | 语音模块的真实用途 | 约束 |
| --- | --- | --- |
| Adapter / Anti-Corruption Layer | Linx、小智、ESP-IDF、Zephyr 的字段和句柄隔离 | 供应商类型不得进入核心头文件 |
| Strategy | PCM/Opus、manual/auto/realtime、WebSocket/MQTT | 每种策略必须有能力声明和契约测试 |
| State | `VoiceSessionState` 与 capture/TTS/interrupt 迁移 | 非法迁移返回 Status，不静默修正 |
| Abstract Factory + Registry | Profile 按 provider ID 创建编译期实现 | 未注册或缺能力必须失败，不自动换 Provider |
| Observer | 产生 VoiceEvidence、ASR/TTS/错误事件 | 证据订阅不能改变会话结果 |
| Strangler Fig | 逐段迁移小智旧实现并保留回退 | 每段有对照测试和删除条件 |

不做一个所有 Adapter 都要继承的 `Plugin` 基类。音频、传输和 Provider 的实时性、所有权与错误语义不同，统一的是 Profile 包络、能力命名和契约测试，不是生命周期细节。

## 7. ESP32-S3 优先的实机验收

在接入破坏性测试前，按以下顺序完成真实板验证：

1. 设备身份、I2S 麦克风和扬声器输出 smoke；
2. 录制 16 kHz PCM，验证帧大小、sequence、generation 和队列水位；
3. WSS hello、listen、ASR（stt）、TTS（tts）和正常 stop；
4. TTS 播放中唤醒/按键打断，确认旧帧不再播放；
5. 拔网、服务端关闭、token 失效和重连，确认本地状态可恢复；
6. 记录固件 Profile、上游 commit、采样参数、延迟、最低空闲堆、原始日志和失败证据。

其他板卡只做以下调研和准入判断：

| 板卡 | 当前判断 | 需要补证据 |
| --- | --- | --- |
| ESP32-C3/C6 | 可作为低资源 Wi-Fi 对照，不替代 S3 主路径 | I2S/PSRAM/AEC/Wake 能力和 Opus 资源预算 |
| ESP32-P4 | 适合高资源音频和视觉扩展 | 音频驱动、功耗和 SDK 版本矩阵 |
| RP2350 Pico 2 | 可验证无 Wi-Fi MCU 的音频/存储边界 | 外部网络协处理器、共享 XIP 写擦与 RTOS |
| STM32H747/GIGA R1 | 适合 Zephyr/HAL 适配器实验 | I2S、网络模块、板级断电和 Codec |
| nRF5340 DK | 适合低功耗/低资源边界 | Opus 体积、RAM 峰值、外部网络与音频输入 |

这些候选均不是当前语音支持声明。ESP32-S3 的易用性、可刷写、可回退和真实音频证据优先于扩展板卡数量。

## 8. TDD 验收

- Red：先让非法 generation、跳号帧、Provider 缺能力、连接失败回滚和打断状态测试失败；
- Green：只实现使契约通过的最小状态机和 Registry；
- Refactor：保持公共 Port 不变，再替换内部队列、Codec 或 WebSocket 实现；
- Integration：Linx/xiaozhi Adapter 解析测试不依赖网络；
- Hardware：ESP32-S3 真机验证采集、上行、ASR、TTS、打断、重连和资源预算，主机绿灯不能代替这些证据。

当前 #106 完成 Port、状态、Provider Registry 和 Linx 协议防腐层；#107 已补上可主机测试、可 ESP-IDF 构建的 WSS Transport 外壳。真实 Linx 云端、Opus、AEC、Wake 和板上闭环仍是明确待办，不能把构建通过写成语音功能已完成。
