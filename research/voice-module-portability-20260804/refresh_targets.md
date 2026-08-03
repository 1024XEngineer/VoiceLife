# 刷新目标

- Linx WebSocket 文档：地址、Header、hello 字段、TTS abort 语义和 MQTT 页面是否补齐。
- `78/xiaozhi-esp32`：上游 AudioService、AFE、Opus 和板级 Codec 的 commit 变化；每次迁移固定 commit。
- ESP-IDF/ESP-SR：I2S、AFE、Opus API 与 ESP32-S3/P4/C3/C6 的目标矩阵。
- SQLite：WAL/rollback journal、`synchronous`、VFS、wear-levelling、文件系统和介质掉电语义。
- ESP32-S3 实板：真实 WSS/ASR/TTS/I2S/AFE/Opus、最低空闲堆、队列水位、重连和物理打断。
- 其他板卡：先做 BoardProfile/CapabilityMatrix 探针；通过准入测试后再进入支持列表。

## 2026-08-04 刷新结果

- Linx 官方仓库目录确认只有 WebSocket 有完整公开协议；`MQTT.md` 当前仍为“待补充”，未发现 HTTP/UDP 设备接入正文。
- ESP-IDF 6.0.2 I2S 文档确认 ESP32-S3 的 standard/PDM/TDM、PDM-to-PCM 端口差异、DMA callback 禁止复杂工作、通道状态和 PM lock 约束。
- ESP32-C3/C6/P4 的 I2S 模式差异已进入能力矩阵；它们仍是候选，不改变 S3 唯一主验证平台。
