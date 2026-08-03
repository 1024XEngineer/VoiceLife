# 刷新目标

- Linx WebSocket 文档：地址、Header、hello 字段、TTS abort 语义和 MQTT 页面是否补齐。
- `78/xiaozhi-esp32`：上游 AudioService、AFE、Opus 和板级 Codec 的 commit 变化；每次迁移固定 commit。
- ESP-IDF/ESP-SR：I2S、AFE、Opus API 与 ESP32-S3/P4/C3/C6 的目标矩阵。
- SQLite：WAL/rollback journal、`synchronous`、VFS、wear-levelling、文件系统和介质掉电语义。
- ESP32-S3 实板：真实 WSS/ASR/TTS/I2S/AFE/Opus、最低空闲堆、队列水位、重连和物理打断。
- 其他板卡：先做 BoardProfile/CapabilityMatrix 探针；通过准入测试后再进入支持列表。
