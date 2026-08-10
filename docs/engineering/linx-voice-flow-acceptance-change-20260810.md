# Linx 语音全流程与本地唤醒验收变更

结论：本轮交付以“设备在本地待机，用户说出你好牛牛后完成一次日程语音闭环并回到待机”为完成定义。`你好牛牛` 是最终真实人声验收项；在它之前先完成并验证 WSS、控制面顺序、MCP、TTS 和待机状态，不能再以启动后持续上传麦克风音频作为可用路径。

下一步动作：按本文的 P0 缺口依次实现、构建和做非人声实板验证；所有 P0 项关闭后，部署人声测试环境，最后执行六次“你好牛牛”试验。本文是对 `linx-mvp-integration-plan-20260810.md` 的增量验收变更，不回改原计划。

## 验收对象与边界

目标用户是放在桌面上的日程设备使用者：设备平时不把环境声音发送给服务端；用户说“你好牛牛”后，才开始本轮语音交互。首个可验收场景是创建日程和查询日程。

本轮做：

```text
启动 -> Linx WSS ready -> 本地 idle
-> 你好牛牛 -> listen.detect -> listen.start -> PCM/STT
-> schedule.create 或 schedule.query -> TTS PCM -> 本地 idle
```

本轮明确不做：

- 默认启用“牛牛”“小牛”等短唤醒词。它们可以作为后续候选命令，但两音节误唤醒率必须先由真实人声和环境数据评估。
- TTS 播放中本地语音打断。当前板卡没有 AEC/playback reference，且播放开始会关闭输入；不能承诺全双工打断。
- Opus 下行解码和 SQLite 日程持久化。这两项不阻塞 PCM 路径验收，但必须在演示与后续任务中如实标为未完成。
- 用云端 STT 命中关键词替代本地声学唤醒。

## 当前链路复核

| 阶段 | 已有事实 | 仍缺的可交付功能 | 验收判定 |
| --- | --- | --- | --- |
| 启动、凭据、OTA | 加密 NVS、OTA bootstrap、真实 WSS 已验证 | 最终部署前的摘要化检查 | 非阻塞；部署脚本必须输出槽位、镜像和分区摘要，不输出秘密。 |
| WSS 与 hello | 官方 WSS、session_id、Ping/Pong 过滤、MCP initialize/tools/list 已有 | 服务端异常恢复的实板证据 | P1；不阻塞首个受控闭环，但失败必须安全停止采集。 |
| 本地待机 | `WakeGateAudioInput` 已有主机级门控与测试 | Runtime 未装配门控；没有具体检测器 | **P0**；idle PCM 不能到 Provider。 |
| ESP-SR 唤醒 | MVP 证明 MultiNet7 的词串为 `ni hao niu niu` | ESP-SR 依赖、模型分区、PSRAM 配置、AFE/检测器实现均未进入当前工程 | **P0**；只注册“你好牛牛”。 |
| 唤醒控制面 | Linx codec 能编码 `listen.detect` | Provider 缺少语义化本地唤醒 API；Runtime 没有控制任务串行执行 `detect` 后 `listen.start` | **P0**；禁止在 I2S 或检测回调里发网络消息。 |
| STT 上行 | 16 kHz S16LE PCM、listen start/stop、session generation 已有 | 真实人声 STT 证据 | **最终项**；不能以静音帧或历史日志代替。 |
| MCP 日程 | `schedule.create`、`schedule.query` 已注册，JSON-RPC bridge 已有 | 真机 `tools/call` 和结果回送证据；数据仍是 mock | **P0** 真机功能验证；“持久化”不在通过条件内。 |
| TTS 下行 | TTS 生命周期、PCM 输出队列、播放时停采集已有 | 服务端真实 PCM 和可听播放证据 | **最终项**。若协商为 Opus，应明确失败而非误报成功。 |
| 回待机 | `VoiceSession` 在 `tts:stop` 后变为 `READY` | 门控未被重新启动；显式结束采集、TTS 结束和异常路径未统一回 idle | **P0**；下一轮唤醒必须无需重启设备。 |
| 观测与隐私 | 只记录生命周期和数值指标的采集脚本已有 | 增加 wake/standby 允许列表事件和真实链路采集记录 | **P0**；不得记录原始 PCM、识别文本、Wi-Fi、token、设备标识或 MCP 参数。 |

## P0 实现顺序

1. 在 `voicelife_audio_esp` 增加 ESP-SR 检测器实现，不新增第四个语音组件。`voicelife_voice` 保持 `LocalWakeDetectorPort` 这个平台无关契约；ESP-SR、AFE 和模型装载细节只留在 ESP 适配层。
2. 以 ESP-SR 2.4.7 的受管依赖和只读 `model` 分区构建模型镜像。分区变更只使用实测的 16 MB 表，在镜像大小、偏移和恢复流程通过检查前不刷入分区表。显式启用并验证 8 MB PSRAM。
3. Runtime 持有物理 PCM 输入、检测器和 `WakeGateAudioInput`，把 gate 交给 `VoiceSession`。删除 hello 后无条件的 `BeginCapture()`，改为 WSS ready 后进入 `StartStandby()`。
4. 为 Provider 增加 `NotifyLocalWakeWord(std::string_view)` 这类语义化接口；Linx 实现发送 `listen.detect`。Runtime 控制任务收到检测事件后严格执行：发送 detect 成功，再调用 `VoiceSession::BeginCapture()`。任一步失败均保持或恢复 idle，不发送 PCM。
5. 将 `EndCapture()`、`tts:start` 后的输入关闭、`tts:stop`、abort 和连接失败统一为可验证的状态转换：正常结束回 idle；无法恢复时保持 failed/不可采集，绝不退化为常开上行。
6. 补主机契约：idle 帧不进 Provider，唤醒事件为 `detect` 再 `listen.start`，TTS/结束采集后恢复 idle，模型或 detect 失败不启动上行。随后完成 ESP-IDF 构建和一次非人声模型装载/WSS/TTS 生命周期验证。

## 最终验收：真实人声

真实人声是最后一项，不在此前用真实语音反复排查硬件。此前可因功能需要刷入 `ota_1`，但每次写入遵守已有备份、摘要校验和恢复流程。

部署完成后，以“你好牛牛”执行以下六次独立试验：

| 试验 | 期望结果 |
| --- | --- |
| 创建日程 1-3 | 每次本地识别后按 `detect -> listen.start` 开始本轮上行；Linx STT 触发一次 `schedule.create`；设备播放对应 TTS，随后回 idle。 |
| 查询日程 1-3 | 每次本地识别后触发一次 `schedule.query`；设备播放对应 TTS，随后回 idle。 |

每次只保存经 `collect_linx_e2e_evidence.py` 允许的生命周期事件、帧计数、延迟、最低空闲堆和结果摘要。通过需同时满足：

- 六次均由本地“你好牛牛”触发，且待机 PCM 没有进入云端上行。
- 每次均出现有序的 wake、capture、STT、MCP、TTS、idle 生命周期；异常不得被计为成功。
- TTS 后无需重启即可再次被“你好牛牛”唤醒。
- 输出可听，且链路未协商到当前不支持的 Opus；没有泄漏敏感信息或用户语音文本的日志/证据文件。

## P1 与后续候选

- 用离线真实语料和环境噪声测试，再决定是否把“牛牛”或“小牛”加入 MultiNet 命令表。通过阈值、距离、环境和误唤醒率应在单独变更中定义，不能凭主观听感直接打开。
- 设计 AEC/reference 后，再单独验证 TTS 期间的本地语音打断。
- 根据 Linx 的实际音频协商记录决定是否另立 Opus decoder 任务。
- 将 Schedule MCP 从 mock 接到 SQLite 后，单独验收跨重启数据一致性。

## 本次缺口结论

现在最缺的不是再做一次板卡压力测试，而是把已有 WSS、MCP、PCM 组件接到“本地待机和受控唤醒”的主状态机。完成 P0 后，才具备开展最后一项真实人声测试的条件。
