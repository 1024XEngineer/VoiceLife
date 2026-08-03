# 小智能力迁移方案

VoiceLife 迁移小智已经验证且难以重写正确的设备能力，但不会复制其整个应用。迁移按防腐层和 Strangler 方式进行：旧能力先包在 Adapter 后面，用相同输入输出做对照，再逐段替换中心化状态和板型耦合。

## 迁移判断

| 小智能力 | 决定 | VoiceLife 落点 | 原因 |
| --- | --- | --- | --- |
| Audio Codec / Audio Service | 迁移 | `AudioDevicePort` Adapter | I2S、Codec、Opus 和队列已有大量硬件细节 |
| ESP-SR AFE / Wake Word | 迁移 | Audio Adapter 子能力 | 唤醒、AEC 和模型配置适合复用 |
| WebSocket Protocol | 迁移并包裹 | `SpeechProviderPort` Adapter | 保留 XRobot 协议兼容，不让协议消息进入 Voice 核心 |
| MCP `tools/list` / `tools/call` | 参考协议，重写边界 | `voicelife_mcp` | 注册和路由可复用思想，业务 Tool 与状态重新实现 |
| Settings / OTA 基础能力 | 后续迁移 | Platform Adapter | 先审计凭据、升级签名和回滚契约 |
| Board 注册与 `config.json` | 收敛迁移 | Adapter Profile | 保留配置构建，移除 138 块板历史矩阵 |
| Display、emoji、camera | 暂不迁移 | 无 | 当前产品路径不依赖，带入只会扩大构建和测试面 |
| 全局 `Application` 状态机 | 不迁移 | Runtime + 独立 Use Case | 它混合音频、协议、UI 和业务生命周期 |
| MQTT/UDP、4G、以太网矩阵 | 暂不迁移 | Transport Adapter 候选 | 当前 PCB 只验证 Wi-Fi + WebSocket |

## 已迁移工具

调研基线为 `78/xiaozhi-esp32@dd99da00dc4c89ed4ab07fcec038c03f13f4de50`。

- `scripts/firmware.py`：从小智 Profile 构建思路收敛而来，支持校验、构建、合并和带 manifest 打包。
- `scripts/audio_debug_server.py`：从 UDP PCM 抓取工具改写而来，显式配置采样率、声道、位宽和输出。

没有迁移：多板 CI 矩阵、OSS 发布、语言资源、LVGL 图片和表情转换。等真实需求进入范围，再从上游按提交固定版本引入。

## 源码迁移顺序

1. **Audio Port 契约**：固定 PCM 格式、缓冲所有权、并发、背压和生命周期；用主机假实现验证 Voice。
2. **Codec Adapter**：只迁移 PCB 实际使用的 Codec 与 I2S 文件，先完成录放音 loopback。
3. **AFE/Wake Adapter**：迁移 AFE 和 `你好牛牛` 模型配置，验证持续唤醒、AEC 参考路径和资源预算。
4. **XRobot Adapter**：迁移 WebSocket 帧、Opus 和重连；在防腐层完成消息转换。
5. **Tool Bridge**：将 XRobot ToolCall 映射到 `voicelife_mcp`，验证 `request_id`、取消和迟到结果。
6. **删旧入口**：对照测试通过后删除被替代的小智 Application 路径，不长期维护双实现。

每一步单独 Issue、单独 PR，可以构建、回退和在真机上验收。迁移 PR 必须列出上游 commit、文件清单、改写点、许可和未迁移依赖。

## 对照测试

- 同一 PCM 输入下，编码格式、采样率、帧长一致。
- 同一协议 fixture 下，XRobot 握手、ToolCall 和 TTS 帧映射一致。
- 网络断开与重连时，不重复提交业务 Tool。
- 新 Generation 启动后，旧代次音频和 ToolResult 不再播放。
- 资源预算记录固件大小、IRAM/DRAM/PSRAM、任务栈和实时丢帧。

不满足这些检查时，迁移只算“代码搬过来”，不算能力迁移完成。
