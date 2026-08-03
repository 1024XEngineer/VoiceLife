# Source 03：xiaozhi-esp32 AFE engine

- URL：<https://raw.githubusercontent.com/78/xiaozhi-esp32/main/main/audio/engines/afe_audio_engine.cc>
- 读取日期：2026-08-04
- 类型：固定上游实现入口

## 原文摘录

> `afe_config_init(input_format.c_str(), models_, AFE_TYPE_VC, AFE_MODE_HIGH_PERF)`

> `afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF`

> `afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM`

> `ProcessingTask may be inside fetch_with_delay() on the same AFE instance ... Defer the reset to ProcessingTask, which owns the fetch side.`

> `generation != control_generation_.load()` 时，旧的 fetch 结果不会被处理。

## 对本项目的迁移判断

WakeNet、MultiNet、VAD、AEC 共用一个 AFE 实例，并由 fetch 所在任务串行执行 enable/disable/reset；控制代次阻断 disable/re-enable 之间迟到的结果。这是 ESP32-S3 Adapter 的所有权契约，不应把 ESP-IDF 的 event group 或句柄泄漏到 Voice Domain。

## 反例记录

旧 MVP 的 `audio/README.md` 仍写着低成本 AEC，但源码实际使用 `AFE_MODE_HIGH_PERF` 和 `AEC_MODE_VOIP_HIGH_PERF`。文档参数必须以源码和实测为准。
