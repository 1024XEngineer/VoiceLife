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

### 2.1 旧 PCB MVP 给出的迁移基线

旧版 `voicelife-pcb-native-mvp` 不是待复制的第二套主干，而是一组已经踩过坑的实验样本。以下参数来自实际源码，迁移时先作为 ESP32-S3 Profile 的起点，再由新板测试决定是否保留：

| 旧版事实 | 新架构中的决定 |
| --- | --- |
| 输入任务每次读取 10 ms、16 kHz PCM，Opus 以 60 ms 组帧 | 采集粒度和网络帧长分开配置；60 ms 是首个 Opus Profile 默认值，不写死为 Voice Domain 规则 |
| 编码、播放队列各容纳 2 个任务，压缩包收发队列各覆盖约 2.4 秒 | 每条队列显式声明容量、满载策略和水位指标；不能只暴露一个没有时间预算的 `queue_size` |
| 麦克风编码队列和网络发送队列满时丢最旧帧，避免网络拥塞反压 AFE | 上行采用 `drop-oldest`；下行播放默认拒绝新帧并上报 underrun/overflow，不允许静默无限等待 |
| `ResetDecoder` 递增播放代次，再清空解码、播放和时间戳队列 | `AudioOutputPort::Flush` 与 `VoiceSession::generation` 必须形成同一个原子语义，迟到解码结果不得重新入队 |
| WakeNet、MultiNet、VAD、AEC 共用一个 AFE 实例；开关和 buffer reset 由 fetch 所在任务串行执行 | AFE Adapter 只有一个所有者任务，控制命令通过 mailbox 进入；禁止控制线程与 `fetch` 并发调用 AFE 句柄 |
| 唤醒前 2 秒 PCM 使用 64 KiB PSRAM 环形缓冲，启动语音处理前预热 120 ms，空闲 15 秒后关闭 ADC/DAC | 环形缓冲、预热和省电阈值属于板卡 Profile；PSRAM 不可用时明确关闭唤醒音频回传，而不是退回堆上无界分配 |
| 输入与输出采样率不一致时分别重采样 | 重采样属于 Codec/Audio Adapter，协商后的实际格式必须随 `AudioFrame` 传播，不能由播放端猜测 |

旧 MVP 自带的 `audio/README.md` 写的是低成本 AEC 配置，但同一份源码实际创建的是 `AFE_MODE_HIGH_PERF` 和 `AEC_MODE_VOIP_HIGH_PERF`。这类说明与代码不一致的参数不进入新 Profile；迁移 PR 必须记录上游 commit、实际宏值、最低空闲堆和丢帧结果，以实测选择配置。

### 2.2 证据不能跨层复用

旧版确定性 PCM 协议测试已经跑通过 `hello -> STT -> ToolCall -> TTS`，也验证过独立的提醒播报和重启恢复；这说明协议、工具和存储链路可以工作。它不等于物理麦克风闭环已经稳定：历史自动化记录中存在“Mac 合成语音未被板载麦克风采集，未产生 ASR”的失败，另一些通过记录也明确注明未执行新的物理麦克风测试。

因此新架构把证据分成四层，前一层不能替代后一层：

1. 主机 fixture：状态、generation、协议解析和错误注入；
2. 云端 PCM：固定音频直连 Provider，验证协议、ASR、Tool 与 TTS；
3. 板上录放：真实 I2S、AFE、Codec、扬声器、队列水位和资源预算；
4. 物理闭环：人在设备前完成唤醒、讲话、工具调用、播报、打断和断网恢复。

PR 只能声明已经拿到证据的层级。#107 已完成第 1 层、ESP-IDF 构建和受控 `ota_1` 启动/恢复验证，但不能继承旧 MVP 的第 2/3 层结果来宣称新 Linx Transport 已完成云端或物理音频闭环。

## 3. 代码契约

### 3.1 稳定值对象

- `AudioFormat`：编码、采样率、声道、位深和帧时长。当前 ESP32-S3 上行首选单声道 16 kHz、16 bit。
- `VoiceAudioFormats`：把 `capture` 与 `playback` 分开。Linx 设备可以用 16 kHz 上行，同时接收服务端协商的 24 kHz TTS；把两者压成一个字段会导致麦克风被错误重开为下行采样率。
- `AudioFrame`：`generation + sequence + format + payload`。generation 用于隔离重连/打断前的迟到帧，sequence 用于发现丢帧和乱序。
- `VoiceSessionConfig`：Provider ID、会话模式、音频偏好、握手超时、重连退避和 MCP 能力开关；不保存 token。
- `CapabilityProfile`：Provider 对外承诺的能力，例如 `streaming-asr`、`tts`、`cancel-generation`、`mcp`、`aec`。
- `VoiceEvidence`：会话 ID、generation、事件和细节。证据只记录状态与标识，不记录 token、原始隐私文本或完整音频。

### 3.2 Port 与实现

| Port | 责任 | ESP32-S3 首个实现 | 其他实现策略 |
| --- | --- | --- | --- |
| `AudioInputPort` | 绑定采集 sink、打开输入、开始/停止采集、关闭 | ESP-IDF I2S + AFE/Wake Adapter | Zephyr I2S、厂商 HAL，先做能力探针 |
| `AudioOutputPort` | 接收解码帧、刷新缓冲、关闭 | ESP-IDF I2S/DAC Adapter | 各板 Codec/扬声器驱动 |
| `VoiceTransportPort` | 连接、文本帧、二进制音频帧、关闭 | TLS WebSocket Adapter | MQTT/UDP 仅在契约满足时接入 |
| `CodecStrategy` | PCM/Opus 编解码 | 小智 Opus 参数迁移 | PCM 直通或其他硬件 Codec |
| `BoundedAudioFrameQueue` | 固定容量、generation 隔离、满载策略和水位统计 | FreeRTOS queue/deque Adapter | Zephyr/NuttX/主机实现，必须复用同一契约 |
| `SpeechProviderAdapter` | Provider 生命周期、采集、播报、打断、能力 | `voicelife_linx` 的 `LinxSpeechProviderAdapter` | `xiaozhi-websocket`、主机 fake |
| `ASRAdapter` / `TTSAdapter` / `RealtimeAdapter` | 外部事件映射与模式差异 | 由 Provider 组合 | 不强迫所有 Provider 继承万能基类 |
| `EvidenceSink` | 记录可关联事件 | 串口/JSON 证据 | CI artifact、真机日志和 JUnit 摘要 |

`AudioInputPort` 的 sink 是单向数据回调，不是会话控制接口。I2S/AFE Adapter 只提交 `AudioFrame.format` 与 `payload`；`generation` 和 `sequence` 必须留空或视为不可信，由 `VoiceSession` 在持有当前采集状态后统一补齐，再调用 Provider。这样板卡驱动不需要知道重连、打断或会话代次，也不会因为复用旧帧元数据而把采集帧误判为新会话。

绑定关系由 `VoiceSession` 拥有：Provider hello 和双向格式协商完成后绑定输入 sink，输入端口打开失败或会话 Stop 时清空 sink。采集结束后，Adapter 可能仍有一个已经排队的回调；会话以 `CAPTURING` 状态和当前格式做最后一道校验，迟到帧只返回错误，不再触碰 Provider 或 SQLite。

### 3.3 会话状态与安全迁移

```text
STOPPED -> STARTING -> READY -> CAPTURING -> READY
                         READY -> SPEAKING -> READY
                         CAPTURING/SPEAKING -> READY (INTERRUPT)
任何启动失败 -> FAILED；Stop 始终回收输入、输出和 Provider
```

状态规则：

1. `Start` 先校验配置并完成 Provider hello，再读取 `VoiceAudioFormats` 打开输入和输出；任何一步失败都按 Output → Input → Provider 回滚，不能让音频硬件先于协议协商定型。
2. `BeginCapture` 同时通知 Provider 和本地输入；输入失败时发送停止，不能留下半开的远端 listen epoch。
3. 输入 sink 只接受当前 capture 格式和非空 payload；会话为每个有效回调补齐当前 generation 和下一个 sequence。手动 `SubmitAudio` 仍要求调用方提供匹配的代次和严格连续的序号。
4. `SubmitAudio` 只接受当前 generation 且严格连续的 sequence；旧 generation 直接丢弃并记录证据。
5. `Interrupt` 发送 Provider abort、刷新输出队列，然后递增 generation；本地不依赖云端迟到的 `tts.stop` 才恢复可用。
6. Transport 断线后立即阻断上行并递增 generation；自动重连只有重新完成 hello 才能从 `STARTING` 回到 `READY`，不会自动恢复上一次采集。
7. 收到 `tts.stop(is_aborted=true)` 时立即 `Flush` 并递增 generation，防止服务端已取消的迟到音频重新进入播放队列。
8. `Stop` 幂等，先让旧 generation 失效，再按 Provider → Output → Input 回收资源并清空输入/下行 sink。

Transport worker 的回调可以与控制任务并发到达：`VoiceSession` 用生命周期锁串行化资源操作，用状态锁把 generation 检查和 `Flush` 绑定起来；证据回调在锁外执行，避免日志或上层观察者反向阻塞实时路径。

### 3.4 有界音频队列契约

`BoundedAudioFrameQueue` 是主机测试与板级 Adapter 共用的最小实现，不把 FreeRTOS queue 类型暴露到核心。构造时必须声明容量和满载策略：

- 上行采集使用 `kDropOldest`。队列满时丢弃最早帧，保留最新语音，累计 `dropped_oldest` 和 `high_watermark`；不能让网络消费者反向阻塞 AFE。
- 下行播放使用 `kRejectNewest`。队列满时拒绝新帧并累计 `rejected_newest`，由 Adapter 转换为 `PLAYBACK_OVERFLOW` 或欠载/降级证据；不能静默覆盖已经排队的 TTS。
- `SetGeneration` 是硬边界：切换 generation 必须清空旧帧；旧 generation 的帧返回 `kConflict` 并计数。
- 队列只接受格式有效且负载非空的 `AudioFrame`。容量、水位、丢帧和拒绝计数必须进入 `VoiceEvidence` 或脱敏测试摘要。

这套契约来自旧 MVP 的 `AudioService` 队列和 `ResetDecoder` 行为，代码位于 [`audio_frame_queue.h`](../../components/voicelife_voice/include/voicelife/voice/audio_frame_queue.h)，主机契约测试位于 [`audio_frame_queue_contract_test.cc`](../../tests/host/audio_frame_queue_contract_test.cc)。

### 3.5 SQLite 与实时音频的边界

SQLite 不作为 `AudioInputPort` 或 `AudioOutputPort` 的同步依赖。MCP/Calendar Application 通过控制面 `StorageTransactionPort` 提交业务命令，Storage Adapter 独占连接并返回 `transaction_id`、提交状态、影响行数、完整性摘要和错误；音频任务只发布异步事件。

Storage Profile 必须同时记录 SQLite 版本、VFS、文件系统、介质、`journal_mode`、`synchronous` 和掉电类型。`commit` 成功只代表该 Profile 的 VFS/同步语义已返回成功，不能替代真实板断电与恢复证据。详见 [语音模块与 SQLite 边界决策](../../research/voice-module-portability-20260804/2026-08-04_decision.md)。

## 4. Linx XRobot WebSocket 防腐层

官方协议（2026-08-04 读取）给出的接入边界如下。协议编解码和 Provider 行为落在 `components/voicelife_linx`，ESP-IDF 的 socket 句柄、TLS、token 解析落在 `components/voicelife_linx_esp`；两层之间只通过 `LinxTransportPort` 协作。Transport 已进入 ESP-IDF 6.0.2 / ESP32-S3 固件构建，并在真实板的非活动 `ota_1` 槽启动过；Runtime 仍未创建生产 Transport，也没有真实 Linx 凭据闭环。

- 默认只接受 `wss://xrobo-io.qiniuapi.com/v1/ws/`；`ws://` 只有在受控内网测试 Profile 显式设置 `allow_insecure_ws` 时才放行，不能作为生产回退。地址也可由 OTA 动态下发。
- 握手头包含 `Authorization: Bearer <token>`、`Protocol-Version: 1`、`Device-Id` 和 `Client-Id`。token 只能来自 `secret://`/NVS/安全配置引用。
- 连接后设备发送 `hello`，声明 `transport=websocket`、MCP 能力和 `audio_params`；服务端返回 hello 后才进入会话。
- 音频二进制帧支持 OPUS 与 PCM。设备 hello 声明上行偏好；服务端 hello 返回下行播放参数，可能是 16/24 kHz 和 20/40/60 ms。Provider 保留上行格式，把下行结果写入 `VoiceAudioFormats.playback`，`VoiceSession` 在 hello 后才打开音频端口。
- `listen(start|stop|detect)`、`stt`、`tts(start|sentence_start|stop)`、`abort` 和 MCP 工具消息均先在 Adapter 映射为 `VoiceEvent` 或 `ToolCall`。
- hello 超时默认 10 秒；异常断线关闭音频发送通道并失效旧 generation。底层 WebSocket 自动重连只恢复传输，Provider 还要对每次新的物理连接补发且只补发一次 hello；hello 完成前不得发送音频。当前只接受服务端保持相同编码，PCM ↔ Opus 变化仍须显式 Codec Strategy，不能静默转码。重连时若下行采样率、声道、位深或帧长变化，Provider 上报错误并让会话保持 `STARTING`，要求上层 `Stop` 后重新 `Start`，当前不做静默 `AudioOutput` 重配置。
- Linx 的 MQTT 页面在 2026-08-04 仍标记“待补充”。架构保留 Transport Port，不实现也不宣称 MQTT 可用。

协议字段不进入 `VoiceSession`。Provider 负责版本、字段类型、最大消息长度、session 绑定和错误码映射；核心只看到 `Status`、`AudioFrame` 和 `VoiceEvent`。

### 4.1 已锁定的离线契约

`LinxJsonCodec` 只负责文档化的控制面消息：

- `hello`：编码版本、WebSocket transport 和音频参数；token 只以 `secret://` 等引用传给 Transport；
- `listen/start|stop|detect`：手动/自动/实时模式和可选 `session_id`；
- `abort`：必须带非空原因，供打断与服务端回放取消关联；
- `hello`、`stt`、`tts/start|sentence_start|stop`、`error`：解析为稳定事件；未知类型、未知 TTS 状态、类型错误和非法音频参数直接失败；
- 二进制帧：由 `LinxSpeechProviderAdapter` 绑定当前连接 generation、单调 sequence 和协商后的下行 `AudioFormat`，再交给会话的 `AudioFrameSink`。
- 生命周期：重复 `connected` 不重复 hello；`disconnected` 立即阻断上行；重连 hello 使用新 generation，旧帧继续拒绝。

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
- Transport 显式上报 connected/disconnected；Provider 在每次 connected 后发送一次 hello。超时、未配置的编码变化或 Transport error 都以 `Status`/`VoiceEvent` 返回，不把半连接状态交给 `VoiceSession`。

这层现在是“可构建、可单测、已完成受控启动、未完成云端闭环”的状态。物理板身份、16 MB Flash、8 MB PSRAM、双 OTA 与数据分区已经在 115200 下读取并备份；新语音固件只写入过 `ota_1@0x410000`，启动成功后已恢复 `otadata` 并确认原固件从 `ota_0` 启动。真实 Linx 凭据、WSS、ASR、TTS 和 I2S/AFE/Opus 仍是下一步。详细守则见 [ESP32-S3 实板变更与恢复](../engineering/esp32-hardware-validation.md)。

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
2. 录制 16 kHz PCM，验证帧大小、sequence、generation 和队列水位；同时确认 24 kHz 下行不会改变上行采集格式；
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
- Red：队列容量为零、旧 generation、上行丢最旧和下行拒绝新帧必须先有失败测试；
- Green：只实现使契约通过的最小状态机和 Registry；
- Refactor：保持公共 Port 不变，再替换内部队列、Codec 或 WebSocket 实现；
- Integration：Linx/xiaozhi Adapter 解析测试不依赖网络；
- Hardware：ESP32-S3 真机验证采集、上行、ASR、TTS、打断、重连和资源预算，主机绿灯不能代替这些证据。

当前 #106 完成 Port、状态、Provider Registry 和 Linx 协议防腐层；#107 已补上可主机测试、可 ESP-IDF 构建、可在非活动 OTA 槽启动并可恢复的 WSS Transport 外壳。真实 Linx 云端、Opus、AEC、Wake 和板上音频闭环仍是明确待办，不能把构建或启动通过写成语音功能已完成。
