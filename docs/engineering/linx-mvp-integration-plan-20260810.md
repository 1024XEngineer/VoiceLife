# VoiceLife Linx MVP 接入计划

## 结论

下一轮只做一条可验证的产品竖切：让当前 ESP32-S3 开发板从真实 Linx OTA 配置出发，完成 WSS hello、PCM 语音往返，并把一组已经存在的日程能力注册为 MCP 工具。第一阶段先证明设备能稳定连接 Linx，第二阶段再证明语音能真正调用日程；Timing 到期推进、IM Gateway 和 Opus/AFE 不作为本轮前置条件。

本计划对应 MS3 的功能闭合目标，作为后续 Issue 和 PR 的共同基线。开始编码后，本文件不回改；需求变化另写变更记录。

## 当前基线

- 目标板：立创实战派 ESP32-S3，芯片 revision v0.2，16 MB Flash、8 MB PSRAM，当前从 `ota_0` 启动。
- 串口：`/dev/cu.usbmodem5A840116301`，固定使用 115200。测试固件只允许写入非活动 `ota_1@0x410000`，结束后恢复 `otadata`。
- `voicelife-pcb-native-mvp` 已经在同一板卡上接入过 Linx，可作为 OTA、WSS、音频任务和回退流程的迁移证据，但不能替代新架构的验收。
- `voicelife_voice` 已有 `VoiceSession`、音频帧边界、generation 和 Provider Registry；`voicelife_linx` 已有协议编解码和 Provider；`voicelife_linx_esp` 已有 ESP-IDF WSS Transport。
- Runtime 目前仍注册 `ScaffoldSpeechProvider`，见 `components/voicelife_runtime/src/runtime.cc`；开发板当前固件只启动架构主干后返回，尚未创建真实 Linx Transport。
- `McpServer` 已完成工具注册、参数校验和调用分发，但当前 Runtime 的工具列表仍为空。
- Schedule 的创建、查询、修改、取消、操作记录和撤销接口已合并，但实现仍使用 mock 数据；真实 SQLite Adapter 尚未接入。
- PR #210 的 Timing 到期推进和 PR #212 的 IM Gateway 属于后续链路，不阻塞本轮 WSS 和语音日程竖切。

## 产品范围

### 本轮要交付

用户对设备说一句简单日程指令，设备能够完成：

```text
设备启动 -> OTA 获取 Linx 配置 -> 控制台绑定 Agent
-> WSS hello -> PCM 采集 -> Linx STT/Agent
-> MCP 日程工具 -> Linx TTS -> PCM 播放
```

首个可演示场景限定为：创建日程、查询日程。工具调用链稳定后，再补修改、取消和撤销。

### 明确不做

- 本轮不接入 Opus、AFE、AEC、WakeNet、物理声学质量调优。
- 本轮不等待 Timing #210 或 IM Gateway #212 完成；提醒触发和微信投递另行验收。
- 本轮不把 mock Schedule 数据写成“真实持久化已完成”。
- 本轮不做多板卡、多 Provider 或 MQTT/HTTP 设备协议。
- 不在代码、Profile、串口日志、备份文件或 Issue 中写入 token、AK、SK、Wi-Fi 密码或业务数据。

## 架构决策

保留三个组件，不合并组件边界：

```text
Runtime
  -> voicelife_voice       会话状态、音频端口、Provider 契约
  -> voicelife_linx        Linx 协议防腐层和 Provider
  -> voicelife_linx_esp    ESP-IDF WebSocket/TLS 实现
```

本轮只收敛 `voicelife_voice` 内的迁移冗余，不重做三层架构：

- 生产路径使用 `VoiceSession + SpeechProviderAdapter`。
- `VoiceSessionCoordinator`、`SpeechProviderPort`、`AudioDevicePort` 等旧接口先不扩展，待真实路径通过后以独立清理 PR 删除。
- `CodecStrategy`、`ASRAdapter`、`TTSAdapter`、`RealtimeAdapter` 暂不实现；没有真实第二 Provider 或 Opus 需求时不增加抽象。
- `voicelife_linx` 只输出稳定的 `VoiceEvent`、`AudioFrame` 和 `ToolCall`，不携带 ESP-IDF 类型。
- `voicelife_linx_esp` 只负责连接、TLS、鉴权 Header、事件队列、分片重组和资源生命周期；Provider 负责 Linx 字段语义。
- Runtime 负责凭据解析、Profile 选择、实例创建和应用层工具注册；业务模块不直接判断 ESP 或 Linx。

## 实现拆分

### PR A：真实 Linx Runtime 装配与 WSS Smoke

关联：Issue #107、#105、#91。

范围：

- 实现 ESP 平台 `SecretResolverPort`，从受控 NVS/secret 引用解析 token。
- 增加 OTA 请求所需的设备身份、Client UUID、固件和板卡信息；保存激活结果的非敏感配置。
- 在 Runtime 注册并创建 `LinxSpeechProviderAdapter` 与 `EspWebSocketTransport`，移除本路径对 Scaffold Provider 的依赖。
- 先验证 WSS、鉴权 Header、客户端 hello、服务端 hello、hello 超时和断线重连。
- 不在本 PR 注册日程工具，不宣称 ASR/TTS 已通过。

验收：

- 主机现有 Linx codec、Provider、fragment 和 VoiceSession 测试全部通过。
- ESP-IDF 6.0.2 / ESP32-S3 构建通过。
- 真实板拿到激活码并完成控制台 Agent 绑定后，串口能记录脱敏的连接状态、hello 成功或明确错误。
- token、完整 Authorization Header、Wi-Fi 信息不出现在日志。
- 连接失败时 Runtime 进入明确失败状态，不启动采集；测试结束板子恢复 `ota_0`。

### PR B：日程 MCP 工具注册

关联：Schedule 已合并 PR #135、#160、#163、#166、#180；MCP PR #155；后续新 Issue。

范围：

- 只注册 `schedule.create` 和 `schedule.query` 两个工具作为第一批真实工具。
- 参数 Schema、必填字段、时间格式、冲突语义和错误结果与 `ScheduleService` 现有契约一致。
- Handler 调用 Schedule Service，不在 Linx Provider 内直接调用日程代码。
- 在 Runtime 启动时冻结工具注册；启动日志只输出工具数量和名称，不输出业务参数。

验收：

- `tools/list` 返回两个工具及完整 Schema。
- 缺参数、类型错误、冲突和未知工具调用均返回稳定错误。
- Host 测试覆盖工具注册、参数校验和 Handler 调用。
- 明确记录数据仍是 mock；不得把工具调用成功写成 SQLite 已验收。

### PR C：语音到日程的真机竖切

关联：PR A、PR B；不依赖 #210、#212。

范围：

- 以 PCM 16 kHz、单声道、16-bit 为上行首选；按服务端 hello 使用下行协商格式。
- 完成 listen start/stop、STT 文本、MCP tool call、TTS start/stop、二进制音频和 abort。
- 采用已有有界队列、generation 和输入/输出隔离；网络不能反向阻塞 I2S 采集任务。
- 首先验收“创建日程”和“查询日程”，其余操作作为下一小步。

验收：

- 真机完成至少各 3 次创建和查询，记录成功、失败、延迟、最低空闲堆、帧丢弃/拒绝计数。
- TTS 播放中打断后，旧 generation 的音频不再播放。
- 拔网、服务端关闭、hello 超时和 token 失效都能得到可诊断状态并恢复或安全失败。
- 保留脱敏串口日志、固件 SHA-256、镜像大小、目标槽和恢复结果。

### PR D：语音模块清理（不阻塞 PR A-C）

范围：

- 删除没有生产调用方的 `VoiceSessionCoordinator` 和迁移期旧 Port。
- 对仍保留的 `CodecStrategy` 等接口给出实际 Provider、Issue 或删除决定。
- 更新架构说明和 Runtime smoke，避免新旧两套语音生命周期同时被误用。

## 真机操作顺序

1. 记录串口、芯片、Flash、PSRAM、分区表和当前启动槽。
2. 115200 备份 `nvs`、`otadata` 及本轮涉及的数据分区，校验字节数和 SHA-256。
3. 构建小于 `ota_1` 分区容量的测试镜像，只写 `ota_1@0x410000`。
4. 运行 OTA 激活和控制台绑定，保存激活码处理结果，不保存 token 明文。
5. 依次观察 WSS、hello、listen、STT、TTS、MCP 和播放证据；失败先保存日志，不重复扩大写入范围。
6. 恢复原 `otadata`，确认重新从 `ota_0` 启动，并复核 NVS、assets、SQLite 数据未改变。

## 风险与停止条件

- 没有真实 Linx 凭据或无法完成 Agent 绑定时，只做主机契约和离线固件构建；不使用 echo/mock 冒充云端通过。
- OTA 返回的 WebSocket 地址、音频参数或 token 形状与当前契约不一致时，停止编码，先记录协议差异再开变更 Issue。
- WSS 可连但 Runtime 仍使用 Scaffold 时，不进入音频闭环验收。
- MCP 工具表为空、Handler 仍调用 mock 以外的未定义路径时，不宣称“语音日程可用”。
- 任意测试出现串口断流、镜像回读不一致、堆破坏或无法恢复 `ota_0`，立即停止真机写入。

## 完成定义

本计划完成的标志不是“代码编译通过”，而是同时满足：

- Runtime 已创建真实 Linx Provider 和 ESP Transport。
- 真机完成 WSS hello，且凭据和日志边界合规。
- MCP `tools/list` 至少提供创建、查询两个日程工具。
- 真机完成语音输入到日程工具再到 TTS 的闭环演示。
- 关键失败路径有 Host 测试和真机证据。
- 所有测试结束后板子、OTA 元数据、NVS 和业务数据可恢复且结果可复核。

## 参考

- [Issue #107：接入 ESP32-S3 Linx WSS Transport 与重连状态机](https://github.com/1024XEngineer/VoiceLife/issues/107)
- [Issue #105：建立可插拔实时音频与 Linx/xiaozhi Provider 契约](https://github.com/1024XEngineer/VoiceLife/issues/105)
- [Linx WebSocket 协议](https://linx.qiniu.com/docs/xrobot/platform/websocket)
- [Linx OTA 协议](https://linx.qiniu.com/docs/xrobot/platform/OTA)
- [语音子架构](../architecture/voice-subarchitecture.md)
- [ESP32-S3 实板变更与恢复](./esp32-hardware-validation.md)
