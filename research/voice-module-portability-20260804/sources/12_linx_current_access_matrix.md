# Source 12：Linx 当前设备接入方式目录

- URL：<https://api.github.com/repos/qiniu/Xrobot-docs/git/trees/main?recursive=1>
- 目录仓库：<https://github.com/qiniu/Xrobot-docs/tree/main/docs/xrobot/platform>
- 读取日期：2026-08-04
- 类型：七牛官方文档仓库目录与一手协议页面

## 实际目录结果

2026-08-04 通过 GitHub 官方 API 读取 `qiniu/Xrobot-docs` 的 `main` 树，`docs/xrobot/platform` 当前包含：

- `websocket.md`
- `MQTT.md`
- `OTA.md`
- `agent-voice-switch.md`
- `blufi-config.md`
- `wifi-config.md`

`MQTT.md` 的正文当前只有“待补充”，仓库中没有可供实现方核对的 MQTT topic、QoS、鉴权、音频包格式或重连时序。当前目录也没有独立的 HTTP 或 UDP 设备接入协议正文。

## 对本项目的约束

1. `LinxTransportPort` 可以保留 WebSocket、MQTT、HTTP 和 UDP 的扩展点，但首个可用实现只能声明已完成契约和测试的 WebSocket。
2. 旧 MVP 的 MQTT + UDP 文档是小智上游协议资料，不是 Linx 官方当前接入承诺；它可以作为迁移参考，不能作为 Linx MQTT 可用证据。
3. Profile 不得写入 `mqtt`、`udp` 的“已支持”能力。未来接入必须先补官方字段来源、离线编解码测试、故障与安全边界，再增加能力声明。

## 限制

目录树证明的是官方资料的公开状态，不证明服务端没有未公开或内测协议，也不证明当前设备凭据可以连通。真实云端闭环仍需脱敏的板上 WSS/hello/ASR/TTS 证据。
