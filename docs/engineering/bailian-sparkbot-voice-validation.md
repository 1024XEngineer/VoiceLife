# 百炼与 SparkBot 语音实板验证手册

结论：本手册定义当前可重复的百炼模型压力测试和 SparkBot 多轮实板验证流程。通过结果只能证明指定模型、网络、固件和板卡组合下的链路质量；SparkBot 当前没有已验证的 playback reference，因此仍按半双工运行，不能据此宣称 AEC 或全双工可用。

适用范围：维护 `scripts/voice_bailian_load_test.py`、`scripts/voice_linx_serial_multiturn_test.py`、Linx 语音接入、SparkBot 音频/显示状态机的开发者。原始串口、音频与云端返回留在受控本地目录，不提交仓库；日志公开边界遵循[硬件调试与串口日志规则](hardware-debugging.md)。

## 1. 测试目标和边界

| 层级 | 工具 | 覆盖 | 不能证明 |
| --- | --- | --- | --- |
| 百炼服务 | `voice_bailian_load_test.py` | TTS、STT、并发虚拟会话、多轮串行回合、文本保真和延迟分位数 | 固件、网络传输、I2S、扬声器和界面状态 |
| SparkBot 实板 | `voice_linx_serial_multiturn_test.py` | 百炼 TTS 注入、板端 STT、Linx 回复、下行 TTS、I2S、状态迁移、字幕和终结语抑制误唤醒 | 麦克风真实声学、AEC 消回声、全双工抢话 |

实板脚本向测试固件的 USB 串口发送 16 kHz、单声道、S16LE PCM。它是状态机和传输压力夹具，不是用户通过麦克风说话的替代品。每个输入句子在首轮开始前全部合成，避免云端 TTS 等待让下一轮已打开的采集窗口空转。

## 2. 每次验证的执行闭环

一次可报告的验证按下表顺序完成。前一层失败时停止向下一层下结论：云端回转写失败先调查模型或网络，不能把问题归到板端；实板失败则保留连续串口，再从状态、队列、I2S 和显示证据定位。

| 顺序 | 工作负载 | 目的 | 通过门禁 | 留存的本地证据 |
| --- | --- | --- | --- | --- |
| 1 | 依赖、密钥和串口占用检查 | 排除环境问题 | `--preflight` 成功，串口独占 | 预检输出、Profile、固件 commit |
| 2 | 百炼 3 并发 x 8 串行回合 | 在没有板端变量时量化模型和网络基线 | 24/24 成功且严格回转写匹配 | 汇总 JSON |
| 3 | SparkBot 八轮上下文会话 | 覆盖注入 PCM、板端 STT、Linx、I2S、状态和字幕 | 第 6 节全部门禁通过 | 连续串口、汇总 JSON |
| 4 | 终结语 8.5 秒观察 | 排除播报余响触发下一轮 | 第 7 节全部门禁通过 | 连续串口、汇总 JSON |
| 5 | 长时、断网和真实声学 | 验证尚未覆盖的长期稳定性与声学边界 | 必须使用本节新增的场景和指标，不得沿用第 2 至 4 步的通过结论 | 受控本地连续日志、原始音频仅在获准时留存 |

第 1 至 4 步是当前可重复基线。第 5 步尚未完成：开始前先明确轮数、网络扰动方式、允许的故障恢复时间，以及堆、队列高水位、任务栈和 I2S 指标的阈值；不要把一次八轮通过包装成长时稳定性或真实 AEC 证据。

## 3. 前置条件

1. 使用带 `CONFIG_VOICELIFE_SERIAL_VOICE_TEST=y` 的 `esp32s3-esp-sparkbot-serial-voice` Profile 构建并刷入 SparkBot；该 Profile 依赖 ES8311、8 MB PSRAM 和已完成 Linx 配置的联网设备。
2. 保证同一时刻只有一个进程打开串口。默认端口是 `/dev/cu.usbmodem14201`，不同主机应在命令中显式传入 `--port`。
3. 主机准备 Python 3、`dashscope`、`pyserial` 和 `ffmpeg`。先执行 `python3 scripts/voice_bailian_load_test.py --preflight`；实板夹具还要求 `pyserial` 和 `ffmpeg` 可用。
4. 从本机受控密钥来源设置环境变量 `DASHSCOPE_API_KEY`，不得把密钥写入命令历史、日志、结果 JSON、文档或 Git。
5. 将输出目录设为本地受控目录，例如 `RESULT_DIR="$(mktemp -d)"`。目录内可保存明文串口和汇总 JSON，完成分析后按本机数据规则清理。

## 4. 默认模型和保真规则

当前严格实板输入基线为 `qwen-audio-3.0-tts-flash + longanlingxi`，板端识别服务为 `qwen3-asr-flash`。选用该组合是因为它在当前句集上通过严格回转写；`cosyvoice-v3-flash + longanhuan_v3` 的一条长句曾稳定把“故事里”识成“卧室里”，即使直接在云端 TTS -> STT 闭环也会出现，不能归因给设备 PCM 或 Linx。

所有严格测试默认按“去掉空白和标点后逐字相同”判定 STT。HTTP/SDK 成功但文本不同仍是失败。仅在专门调查模型误识时才可加 `--allow-transcript-mismatch`，并必须在结论中标为不满足保真门禁；不得对特定词做替换或特判。

## 5. 百炼服务压力测试

先验证本机依赖和密钥状态：

```bash
python3 scripts/voice_bailian_load_test.py --preflight
```

运行 3 个并发会话、每个会话连续 8 轮的严格回转写压力：

```bash
RESULT_DIR="$(mktemp -d)"
python3 scripts/voice_bailian_load_test.py \
  --requests 3 \
  --concurrency 3 \
  --turns-per-conversation 8 \
  --tts-model qwen-audio-3.0-tts-flash \
  --voice longanlingxi \
  --stt-model qwen3-asr-flash \
  --result-json "$RESULT_DIR/bailian-load.json"
```

通过条件：请求全成功、每个会话 8 轮都完成、`transcript_matches == successful`，并记录 TTS、首包、STT、端到端和每个完整会话的 p50/p95/p99。该脚本刻意让同一虚拟会话内的回合串行，后续回合会承受前一轮的服务负载；不同会话按 `--concurrency` 并行。

出现失败时，保留汇总 JSON 和不含密钥的错误类别（例如 `tts:timeout`、`stt:http:429`），区分服务端超时、限流、网络和保真失配。不要将失败的音频、Authorization 头或密钥写入仓库。

## 6. SparkBot 八轮上下文实板测试

启动前确认设备已联网、Linx 已连接且串口未被其他程序占用。以下会话刻意包含指代、顺序复述、长回复和两句话约束，可同时覆盖上下文连续性和字幕滚动路径：

```bash
RESULT_DIR="$(mktemp -d)"
python3 scripts/voice_linx_serial_multiturn_test.py \
  --port /dev/cu.usbmodem14201 \
  --tts-model qwen-audio-3.0-tts-flash \
  --voice longanlingxi \
  --require-display-scroll \
  --serial-log "$RESULT_DIR/sparkbot-context8.log" \
  --result-json "$RESULT_DIR/sparkbot-context8.json" \
  --text '我在读一篇关于海边灯塔的短文，请先记住主角是一位守灯人。' \
  --text '故事里守灯人每天傍晚都会检查灯塔的玻璃和灯芯。' \
  --text '请用一句话说出到目前为止是谁在做什么。' \
  --text '后来海上起雾，他决定先点亮备用灯，再去查看码头。' \
  --text '请把目前发生的两件事按顺序复述。' \
  --text '清晨雾散了，他看到一只红色小船安全靠岸。' \
  --text '请说明红色小船出现前后故事有什么变化。' \
  --text '最后请用两句话总结这个故事的线索，但不要添加新的角色。'
```

脚本要求每一轮按以下顺序出现：`capture_stopped`、`stt_text_received`、`tts_started`、首个 TTS 音频与首句显示、`tts_stopped`，随后自动重开下一轮采集。它还检查：

- 8/8 回合完成且 STT 严格匹配输入；
- `AUDIO_STATS` 中输入/输出帧存在，`in_drop`、`out_reject`、`short_write`、`in_i2s_err`、`out_i2s_err` 均为零；
- 没有 `SERIAL_VOICE_PCM=reject`、`provider_error` 或 `INTERACTION_REJECTED`，交互队列三类丢弃均为零；
- 状态机和显示都覆盖 phase `3/4/5/6`；
- 每个 `SPARKBOT_TEXT_RENDER` 都含 generation、revision、可视区和原始文本，内容可视区固定为 50 px 两行；`--require-display-scroll` 还要求至少一次真实纵向滚动。

测试日志中的普通应用文本、状态和计数可以保留明文以支持诊断；其中出现的凭据、个人数据或原始私密音频必须删除后才能公开。

## 7. 终结语与误唤醒观察

终结语可能包含本地命令词。无 AEC 板型在终结 TTS 后必须保持 8 秒 wake guard，不能把扬声器余响重新当作唤醒。单独执行：

```bash
RESULT_DIR="$(mktemp -d)"
python3 scripts/voice_linx_serial_multiturn_test.py \
  --port /dev/cu.usbmodem14201 \
  --tts-model qwen-audio-3.0-tts-flash \
  --voice longanlingxi \
  --expect-terminal \
  --guard-observation-seconds 8.5 \
  --serial-log "$RESULT_DIR/sparkbot-terminal.log" \
  --result-json "$RESULT_DIR/sparkbot-terminal.json" \
  --text '今天的故事我已经记住了，谢谢你，再见。'
```

通过条件：出现 `WAKE_GUARD_ARMED ms=8000 reason=terminal_tts`，其后的 8.5 秒内没有 `WAKE_DETECTED`、新一轮 `SERIAL_VOICE_CAPTURE_READY` 或新的本地回应；其余音频、状态和显示门禁也必须全部通过。

## 8. 结果记录与已验证基线

每次测试在 PR 或 Issue 中至少记录下列字段。原始音频、完整串口和任何秘密仍只留本地；公开记录只放足以复核结论的非敏感片段和汇总。

| 类别 | 必填字段 |
| --- | --- |
| 环境 | 日期、板卡、串口参数、Profile、固件 commit、ESP-IDF 版本、Linx 连接状态、模型和 voice |
| 工作负载 | 运行命令、会话数、每会话回合数、输入句集、是否要求滚动和终结语观察 |
| 百炼结果 | 成功数、严格匹配数、错误类别、TTS 首包与端到端 p50/p95/p99 |
| 实板结果 | 每回合状态顺序、显示 phase、滚动次数、输入/输出帧、队列丢弃、I2S 短写/错误、PCM reject、Provider/交互错误 |
| 人工观察 | 是否实际听到下行回复；只能陈述观察到的物理事实，不能外推声学质量或 AEC 效果 |
| 结论 | 通过或失败、失败的首个门禁、连续日志位置、下一步根因调查或回归命令 |

2026-08-19 的受控实板验证在 SparkBot（ES8311、8 MB PSRAM）上得到以下结果：

- 八轮上下文会话完成 8/8，STT 严格匹配 8/8；音频零丢失、无 PCM reject、无 Provider/交互拒绝，状态与显示门禁全通过，6 条长文本进入两行滚动。
- 终结语测试通过：wake guard 持续 8 秒，观察 8.5 秒内没有误唤醒。
- 人工在实板扬声器实际听到回复中的“红色小船安全靠岸”。这证明当前测试链路产生了可听见的物理播放；它不代替声压、失真、麦克风回采或双讲声学测量。
- 百炼的 `cosyvoice-v3-flash + longanhuan_v3 + qwen3-asr-flash` 已完成 3 并发 x 8 轮、24/24 成功且 24/24 保真的严格压力基线；Qwen 组合已完成关键句 5/5 云端回转写和 SparkBot 八轮实板闭环。此前 CosyVoice 组合的特定长句失配应保留为模型组合的已知限制，而不是以设备端词表规则掩盖。

这些数值是当前基线，不是永久 SLO。每次更换模型、固件、Linx 协议、音频帧格式、网络或板卡后都必须重新执行第 5 至第 7 节，并在对应 PR 记录新的模型名、命令、汇总结果、固件 commit 和剩余风险。

## 9. 当前不通过的情况和后续验证

以下任一项发生即本轮失败：任意文本失配、队列丢帧、I2S 短写/错误、迟到 PCM 被接受、状态/显示缺相、滚动路径未覆盖、终结语误唤醒，或外部服务报错。失败后先保留连续原始串口，按 generation、队列高水位、WebSocket 写失败、I2S 统计和显示 revision 定位根因；修复必须面向同类机制，不能只为某一句话添加分支。

当前尚未验证的项目：真实麦克风长时声学压测、断网重连、真实播放 reference、ESP-SR AFE/AEC ProcessingTask、双讲打断和 `played_ms` 截断。没有上述证据前，SparkBot 的默认交互保持“播放中不自动语音抢话；通过显式停止、按键或明确唤醒先停止播放后再进入聆听”。
