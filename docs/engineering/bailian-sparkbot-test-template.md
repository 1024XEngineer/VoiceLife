# 百炼 SparkBot 固定测试模板

结论：以后所有百炼到 SparkBot 的验证都从 `scripts/run_bailian_sparkbot_test.sh` 进入；它会安全读取 Key、先做真实 TTS 预检，再执行唤醒或多轮串口测试。这样不会再把 `key.txt` 中的 API 地址误当成 API Key，也不会把“脚本启动”当成链路通过。

下一步：插好 SparkBot 后先运行 `preflight`，预检通过再运行 `wake`；需要上下文时再运行 `multiturn`。每次把脚本生成的 JSON、`.meta.txt` 和串口 `.log` 一起留在本机证据目录。

## 1. 固定入口

```bash
cd /Users/mac/Desktop/project/VoiceLife

# 只验证 Key、SDK 和百炼 TTS 的真实连通性，不打开串口
scripts/run_bailian_sparkbot_test.sh preflight

# 用百炼 TTS 合成“你好牛牛”，通过 USB 串口注入 SparkBot
scripts/run_bailian_sparkbot_test.sh wake

# 在同一 Profile 下执行多轮上下文；可重复传入 --text 覆盖默认句集
scripts/run_bailian_sparkbot_test.sh multiturn \
  --text '请记住今天的主题是日程管理。' \
  --text '把刚才的主题复述一遍。' \
  --text '请用一句话总结我们刚才谈了什么。'
```

`preflight` 会真实调用一次 TTS，并在日志目录写入 `preflight-*.json`；只有 JSON 中 `failed` 为 `0` 且 `audio_bytes.total` 大于 `0`，才算百炼可用。`wake` 和 `multiturn` 还会检查串口、`pyserial` 和 `ffmpeg`。

## 2. 配置读取

本项目本地配置文件可以同时包含兼容 API 地址和密钥，例如：

```text
https://.../compatible-mode/v1
https://.../apps/anthropic
sk-...
```

它不是 dotenv 文件。只读取第一条完整的 `sk-` 行：

```bash
KEY_FILE="/Users/mac/Desktop/project/语音模型调用/key.txt"
export BAILIAN_KEY_FILE="$KEY_FILE"
scripts/run_bailian_sparkbot_test.sh preflight
```

脚本内部会去掉行首尾空白和 CR，只取第一条完整的 `sk-...` 行。等价的读取逻辑是：

```bash
awk '{ line=$0; gsub(/^[[:space:]]+|[[:space:]]+$/, "", line); if (line ~ /^sk-[[:alnum:]_-]+$/) { print line; exit } }' "$KEY_FILE"
```

禁止使用 `DASHSCOPE_API_KEY="$(< key.txt)"`，也禁止把 Key 写入日志、命令输出或 Git。仓库脚本 `scripts/run_bailian_sparkbot_test.sh` 已内置同样的解析和检查逻辑。

## 3. 固定 Profile

| 项目 | 默认值 |
| --- | --- |
| TTS | `qwen-audio-3.0-tts-flash` |
| 音色 | `longanlingxi` |
| 设备 | SparkBot 实板 |
| 串口 | `/dev/cu.usbmodem14401` |
| Linx 音频 | PCM、16 kHz、单声道、16 bit、20 ms |
| 日志目录 | `/tmp/voicelife-bailian-tests` |

如果切换模型、音色、串口或地域，必须在测试记录中显式写出新值；不要只改命令而不改证据。

## 4. 预检与唤醒

不要再直接拼接 `DASHSCOPE_API_KEY` 或直接调用带有另一组默认模型的底层脚本。固定预检命令是：

```bash
cd /Users/mac/Desktop/project/VoiceLife
scripts/run_bailian_sparkbot_test.sh preflight
```

预检通过后再执行实板唤醒：

```bash
cd /Users/mac/Desktop/project/VoiceLife
scripts/run_bailian_sparkbot_test.sh wake
```

唤醒通过必须同时出现：

```text
SERIAL_VOICE_EVIDENCE event=standby_ready
WAKE_DETECTED word=你好牛牛
SERIAL_VOICE_EVIDENCE event=local_wake_ack_requested
SERIAL_VOICE_EVIDENCE event=tts_started
SERIAL_VOICE_EVIDENCE event=tts_stopped
LINX_SEND listen state=start mode=auto
SERIAL_VOICE_EVIDENCE event=capture_started
```

小智 SparkBot 的参考实现关闭 AEC 时使用 `auto`，并且在
`CONFIG_SEND_WAKE_WORD_DATA=y` 时先发送 `listen.detect`；它只有开启设备或
服务端 AEC 才切换 `realtime`。Linx 文档虽然推荐 `realtime`，但 VoiceLife
当前使用 `auto`，因为 SparkBot 没有 AEC、VoiceLife 也没有经过验证的本地
回采打断能力。SparkBot 的本地 MultiNet 没有唤醒词 Opus 缓存，
但 Linx 仍要求先发送 `listen.detect` 建立会话；本链路只携带唤醒词，并请求
一次短确认音。确认音结束后才发送 `listen.start(auto)`，屏幕再进入“聆听中”。
这样避免服务端欢迎音频与首轮 PCM 交错，降低 Linx 会话边界断连。
“别说了”打断和定时提醒仍可以单独使用正式的远端 TTS。

唤醒脚本用 `WAKE_BEGIN` 的请求/响应确认测试任务和待机状态，不把只在固件启动时打印一次的
`SERIAL_VOICE_TEST_READY=1` 当作每次串口连接的就绪信号。因此设备已经运行、重新打开串口时也可以重复执行；
显式传入 `--reset-before-run` 时则会先通过 USB-Serial/JTAG 复位，再等待同一个握手。

若出现 `STARTUP_ERROR stage=session_start`、`provider_connect_failed` 或 `Connection reset by peer`，先标记为 Linx 连接失败并重试启动，不得把它写成“唤醒词识别失败”。

## 5. 普通对话和多轮

唤醒通过后，用同一套模型运行多轮上下文和日程语音测试。最小多轮模板：

```bash
cd /Users/mac/Desktop/project/VoiceLife
scripts/run_bailian_sparkbot_test.sh multiturn \
  --text '请简单介绍一下你自己。' \
  --text '把刚才的回答缩短成一句话。' \
  --text '请说明我们刚才谈了什么。'
```

每轮必须记录并检查 `capture_stopped`、`stt_text_received`、`tts_started`、`tts_first_audio`、`tts_stopped`，以及屏幕的 `聆听中/处理中/说话中` 状态。日程验收在同一 Profile 下追加：创建、查询、修改、查询修改结果、删除、删除后查询；标题使用唯一值，避免历史数据干扰。

## 6. 重启与重连

1. 保存脚本产生的串口 `.log` 和 `.json`，记录固件 commit、Wi-Fi、Linx hello 和 `AUDIO_STATS`。
2. 硬重启后等待 `standby_ready`，再次执行 `wake`，确认唤醒和普通对话仍成功。
3. 观察一次 WebSocket 断线后的 `transport_disconnected`、重连和新的 `transport_connected`，再执行唤醒。
4. 任何 `in_drop`、`out_reject`、`short_write`、I2S 错误、`INTERACTION_REJECTED` 或 MCP 响应超时都要单列，不能用“最终播报了”掩盖。

## 7. 证据记录

脚本默认将原始串口日志、JSON 汇总和不含密钥的 `.meta.txt` 写入 `/tmp/voicelife-bailian-tests`。PR/Issue 只粘贴时间、模型、固件、命令、成功/失败 marker 和汇总计数；密钥、Authorization、原始私密音频不进入仓库。每次测试都复制以下小表并填写：

```text
日期/固件:
板卡/串口:
TTS 模型/音色:
唤醒: PASS/FAIL（日志）
普通对话: PASS/FAIL（轮数）
多轮上下文: PASS/FAIL（轮数）
日程 CRUD: PASS/FAIL（创建/查/改/删）
重启后唤醒: PASS/FAIL
重连后唤醒: PASS/FAIL
异常计数: in_drop= out_reject= short_write= i2s_err= queue_drop=
未决问题:
```
