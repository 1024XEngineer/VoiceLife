# Source 10：voicelife-pcb-native-mvp AFE 与唤醒缓存

- 本地文件：`/Users/mac/Desktop/project/voicelife-pcb-native-mvp/firmware/main/audio/engines/afe_audio_engine.cc`
- 读取日期：2026-08-04
- 类型：本地实现与实板证据

## 关键行

- AFE 初始化使用 `AFE_MODE_HIGH_PERF`、`AEC_MODE_VOIP_HIGH_PERF`，关闭 NS，并优先从 PSRAM 分配。
- `ProcessingTask` 独占 `fetch_with_delay`、reset 和 AFE 开关，控制代次拒绝迟到结果。
- 唤醒前音频缓存使用固定容量 PSRAM 环形缓冲；初始化失败时关闭上传能力，不退回无界堆分配。

## 新架构保留与删除

这些是 ESP32-S3 Profile 的实现约束，不是跨平台 Domain API。板卡适配必须报告 AFE 模式、最低空闲堆、缓存是否启用和 fetch/reset 代次证据。
