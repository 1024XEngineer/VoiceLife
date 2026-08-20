# SparkBot 语音缓冲回归记录（2026-08-19）

结论：八轮真实 SparkBot 语音-状态机-屏幕链路首次暴露了长回复下的通用下行背压问题；将有界抖动缓冲从 240 ms 调整为 320 ms 后，第二次八轮长上下文压测通过，输出帧拒绝从 2 降为 0。生产默认音量仍为 70，本次 `esp32s3-esp-sparkbot-serial-voice` 测试固件固定为 35。

## 环境

- 硬件：SparkBot，ESP32-S3，8 MB PSRAM，ES8311，USB-Serial/JTAG `/dev/cu.usbmodem14201`
- ESP-IDF：6.0.2
- Profile：`esp32s3-esp-sparkbot-serial-voice`
- 固件测试输出音量：ES8311 和 PCM 数字缩放均为 35；生产 Profile 未改变 70
- 网络：`Qiniu-Guest`，Linx WebSocket 已连接
- 原始串口和脚本结果：仅保存在 `/tmp`，不提交仓库

## 首次失败证据

八轮长上下文会话全部完成，且 STT、状态机和屏幕门禁通过，但日志显示：

```text
out_q_cap=12 out_latency_budget_ms=240 out_q_hi=12 out_reject=2
```

拒绝发生在 `Esp32s3PcmAudioPorts::PushOutput` 的有界队列准入处。Linx 可以在一个网络突发中交付超过 12 个 20 ms PCM 帧，旧实现将 `kConflict` 静默视为可丢弃帧，造成可听见的下行 PCM 缺口。该问题与句子内容无关，也不是日程业务错误。

同一轮的其他证据：8/8 STT 严格匹配，`in_drop=0`，`short_write=0`，`in_i2s_err=0`，`out_i2s_err=0`，状态 phase 3/4/5/6 完整，双行滚动真实触发。

## 修复

SparkBot 仍使用有限队列，不改成无界缓存：

- 输出队列深度：12 -> 16 帧
- 最大播放延迟预算：240 -> 320 ms
- 拒绝路径新增 `OUTPUT_REJECT` 结构化日志，记录原因、排队帧数、排队时长、入帧时长、容量和预算
- 测试夹具新增 `--reset-before-run`，通过 USB-Serial/JTAG RTS 显式复位后等待完整 ready 序列，避免人工启动时序污染结果

320 ms 仅增加 80 ms 有界抖动余量；输出任务仍按 FIFO、I2S 实时消费，打断时仍由 `Flush` 清空队列。

## 修复后实板结果

使用与首次失败完全相同的八轮输入句集，运行：

```bash
/tmp/voicelife-bailian-venv/bin/python scripts/voice_linx_serial_multiturn_test.py \
  --port /dev/cu.usbmodem14201 --reset-before-run --input-tts macos-say \
  --require-display-scroll --serial-log /tmp/voicelife-volume35-context8-buffer320.log \
  --result-json /tmp/voicelife-volume35-context8-buffer320.json
```

结果：

| 指标 | 结果 |
| --- | ---: |
| 回合完成 | 8/8 |
| STT 严格匹配 | 8/8 |
| 输入/输出帧 | 8002 / 4820 |
| 输出队列峰值 | 10/16 |
| 输出帧拒绝 | 0 |
| 输入丢弃、串口 PCM 拒绝 | 0、0 |
| I2S 短写/输入错误/输出错误 | 0、0、0 |
| 交互队列丢弃 | 0 |
| 屏幕文本快照/内容快照 | 72 / 41 |
| 真实字幕滚动 | 是 |
| 测试输出音量 | 35 |

所有脚本 acceptance 门禁均为 true；日志未出现 `OUTPUT_REJECT`、`INTERACTION_REJECTED`、`SERIAL_VOICE_PCM=reject`、栈溢出、Guru Meditation 或新的复位。

## 本地门禁

- `./scripts/check_format.sh ...`：通过
- `./scripts/run_host_tests.sh`：80/80 通过
- `./scripts/check_architecture.sh`：通过
- `python3 scripts/firmware.py validate`：全部 7 个 Profile 通过
- ESP-IDF `python3 scripts/firmware.py build esp32s3-esp-sparkbot-serial-voice`：通过，应用分区余量 26%

## 边界与后续

- 百炼 TTS SDK 的 WebSocket 建连在本机出现 5 秒超时；HTTP 域名可连接。该外部网络问题不能归因于固件，实板本轮使用本地 TTS 生成 PCM 作为输入夹具，板端 STT、Linx 下行 TTS、I2S、状态和屏幕仍为真实链路。
- 本记录证明的是协议/队列/状态/显示稳定性，不证明真实扬声器回采、AEC、双讲或真实麦克风长时声学质量。下一阶段必须在允许的声学环境中补做播放 reference、AEC/双讲、断网重连和更长时长压测。
