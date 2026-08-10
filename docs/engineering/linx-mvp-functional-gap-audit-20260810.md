# Linx MVP 全流程功能缺口审计

结论：当前固件已经具备安全取配置、WSS hello、MCP 工具注册和 PCM I2S 数据面，但尚不能作为“本地唤醒后可连续使用的语音日程设备”验收。最先要补的是待机状态与本地唤醒，随后用真实人声完成 STT、MCP、TTS 与恢复链路；不能以构建、静音连接或历史 MVP 日志替代。

下一步动作：按本表先补确定的状态机和格式能力，再部署“你好牛牛”作为最终人声验收项。多唤醒词和语音打断不与首版混合交付。

## 审计范围和证据

本次按 `linx-mvp-integration-plan-20260810.md` 的产品路径检查当前工作树，并运行以下可重复门禁：

- `./scripts/run_checks.sh`：41 个 C++ 主机测试、53 个 Python 测试、公共 API、架构边界、契约版本和 Profile 校验全部通过。
- ESP-IDF `v6.0.2`：`python3 scripts/firmware.py build esp32s3-voicelife-pcb-pcm` 通过；`voicelife.bin=0x170bf0`，最小 OTA app 分区余量 63%。
- 实板历史受控证据只证明 Wi-Fi、OTA、WSS、hello、MCP initialize/tools-list、I2S 数字 PCM、保活和恢复流程；不包含可审计的真实语音内容或听感结论。

## 全流程结果

| 阶段 | 当前实现 | 直接证据 | 缺口或结论 |
| --- | --- | --- | --- |
| 启动与凭据 | 加密 NVS 读取 Wi-Fi，HMAC provider 后执行 OTA | Runtime 与 OTA bootstrap；历史实板启动 | 已有。仍缺最终部署脚本的一键状态检查。 |
| OTA 与 WSS | 官方 OTA 响应生成 WSS 配置；Transport 过滤 Ping/Pong，hello 绑定服务端 session | 主机契约和历史 WSS 保活 | 已有，不应重复刷板做无目的稳定性测试。 |
| 音频协商 | 上行 16 kHz PCM；下行 PCM 支持协商 16/24 kHz | Provider 和 I2S PCM 构建 | 若 Linx 返回 Opus，当前会安全失败；没有 Opus decoder。 |
| 待机 | Runtime 在 hello 后立即调用 `VoiceSession::BeginCapture()` | `runtime.cc` | 缺失：没有 idle、本地检测、节流或唤醒入口；麦克风一直向云端开启。 |
| 本地唤醒 | 当前没有 ESP-SR/AFE/MultiNet 依赖或模型分区 | Profile、依赖锁和 Runtime | 缺失。首版目标为本地“你好牛牛”，作为最终验收项。 |
| 唤醒到云端 | Linx 已有 `listen.detect` 编码，但没有本地检测器调用它 | `LinxJsonCodec::EncodeListenDetect` | 缺失：需要保证 detect 在 listen.start 前且不在 I2S 任务内阻塞网络。 |
| 上行 STT | PCM 帧、generation 和 listen start/stop 已实现 | VoiceSession、Provider 契约 | 缺失真机人声 STT 证据。 |
| MCP | `schedule.create`、`schedule.query` 已注册，JSON-RPC initialize/tools/list/call 已桥接 | 主机 MCP 测试、历史 tools/list | 缺失真机 tools/call 证据；数据仍是 mock，不是 SQLite 持久化。 |
| TTS 数据面 | tts 生命周期和 PCM 下行进入 I2S 输出队列 | 主机契约、I2S 数字回放 | 缺失真实服务端 PCM 下行及外部可听声验证。 |
| TTS 后恢复 | `tts:start` 会停止输入；`tts:stop` 回到 `READY` | VoiceSession | 缺失：没有恢复本地待机检测或下一轮唤醒的状态迁移。 |
| 本地语音打断 | 无实现 | 物理 Profile `input_reference=false` | 首版不做。无 AEC 板上播放期间继续采集会有扬声器回声误触发风险。 |
| 网络/服务故障 | 断线会失效 generation 并进入 starting；Provider 等待新 hello | VoiceSession/Provider 主机测试 | 缺失真机拔网、token 失效、服务端关闭后的恢复证据。 |
| 数据安全与恢复 | 凭据不写普通 NVS；测试写非活动槽后恢复原镜像 | 历史受控备份/回读流程 | 已有基线；最终人声环境部署时再次执行并记录摘要。 |

## 首版范围决定

本轮补齐的最小闭环是：

```text
启动 -> WSS ready -> 本地 idle
-> 你好牛牛 -> listen.detect -> listen.start -> PCM/STT
-> MCP create/query -> TTS PCM -> idle
```

首版不做以下内容：

- “牛牛”“小牛”等短词默认启用；MultiNet 可支持多命令，但短词需要误唤醒数据后才允许进入默认配置。
- TTS 播放中的本地语音打断；该板没有 AEC/reference，不能用未验证的全双工冒充可用。
- Opus 下行解码；收到 Opus 时继续明确失败，待拿到真实协商数据后单独设计。
- SQLite 日程持久化；当前 MCP 成功只代表 mock `ScheduleService` 成功。

## 实现与验收顺序

1. 引入独立 ESP-SR MultiNet7 检测器和模型只读分区，显式启用 PSRAM；只注册“你好牛牛”。
2. 为现有 PCM 输入增加门控：idle 帧仅送本地检测，唤醒后才转交 VoiceSession；检测回调经控制任务发送 `detect` 和启动 listen，不能在 I2S worker 内发网络消息。
3. TTS 停止和捕获结束后回到 local idle；断线、模型加载失败和 format 不匹配均不得留下持续采集。
4. 增加主机状态/顺序契约、ESP 构建和单次非人声模型装载验证。
5. 最后部署到 `ota_1`，以“你好牛牛”各完成 3 次创建与查询；同时收集脱敏的 wake、STT、MCP、TTS、帧计数、延迟、最低堆和恢复摘要。
