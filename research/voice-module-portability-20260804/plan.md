# 语音模块可迁移性调研计划

一句话结论：本次调研用于决定 ESP32-S3 语音链路哪些经验应进入稳定架构，哪些只能留在板级 Adapter；同时为 SQLite/业务写入与实时音频隔离提供证据。
一句话动作：先以 ESP32-S3 为唯一可用 Profile，冻结音频帧、队列、generation 和 Provider 契约，再用能力探针评估其他板卡，不把“能编译”升级成“已支持”。

## 1. 研究问题

1. `voicelife-pcb-native-mvp` 和 `78/xiaozhi-esp32` 中哪些音频任务、队列、AFE 和采样率处理值得迁移？
2. Linx WebSocket 的握手和事件边界如何放进 Provider 防腐层？
3. 如何保证网络、SQLite 和业务编排不会反向阻塞麦克风与播放？
4. 怎样定义可迁移到其他 MCU/RTOS 的最小公共契约？

## 2. 可证伪假设

- H1：实时音频必须与协议、业务和 SQLite 分成不同执行面；否则网络抖动会造成采集反压或播放打断。
- H2：ESP32-S3 的首个生产 Profile 需要单一 AFE 所有者任务、独立 capture/playback 格式和有界队列。
- H3：跨板卡的稳定抽象应是 AudioFrame/Port/Profile/Capability，而不是复制小智的板卡目录和全局状态机。
- H4：SQLite 写入应通过业务控制面队列和统一事务协议进入存储 Adapter，不能从音频任务直接调用。

## 3. 取证策略

优先使用七牛 Linx、Espressif、SQLite、Zephyr 等官方文档；再用固定 commit 的小智源码和本地 MVP 源码做实现交叉验证。每条关键判断至少绑定一个协议/平台主来源和一个实现来源；无法证明实板能力的内容标记为“待实测”。

## 4. 停止条件与风险

- 只要能确定上行/下行格式、hello 生命周期、队列满载和 AFE 所有权，先冻结主机契约，不等待其他板卡资料齐全。
- 浏览器工具不可用时，使用官方 raw GitHub/文档 HTML 只读抓取，并保存 URL、日期和逐字摘录。
- 不抓取或保存 token、AK/SK、Wi-Fi 密码、原始隐私语音和完整云端会话。

## 5. 变更记录

- 2026-08-04：吸收旧 MVP 的音频任务、PSRAM 环形缓存、AEC/AFE 控制代次和实板证据边界。
- 2026-08-04：新增平台无关 `BoundedAudioFrameQueue` 主机契约，作为 ESP32-S3 Adapter 的下一步接入点。
