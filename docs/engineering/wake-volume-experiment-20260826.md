# SparkBot 唤醒与音量实板实验记录（2026-08-26）

本记录固定本轮固件、实板和已观察到的现象，作为 PR #377 的验证证据。结论是：告别后的唤醒困难由 8 秒本地回声保护窗直接造成；音量调节实际成功，但此前被 MCP 桥接层错误显示为失败。

## 环境

- 固件 Profile：`esp32s3-esp-sparkbot-serial-voice`
- ESP-IDF：6.0.2
- SparkBot MAC：`98:a3:16:c7:81:b0`
- Wi-Fi：`zxp`
- 应用镜像：`build/esp32s3-esp-sparkbot-serial-voice/voicelife.bin`
- 应用镜像 SHA-256：`c39db3e341ce3ec843feaa0d8b437b97b98157688f76785c6fe432d0ba3778cc`
- 刷写方式：仅写入应用分区 `0x10000`，保留 Wi-Fi/NVS、assets、model 和 SQLite

## 本轮代码改动

1. `ToolResultStatus` 以 `ToolResult::Success` 作为标量/空结果的成功依据；结构化结果只有显式 `conflict`/`failure` 才覆盖成功。
2. 新增布尔 MCP 结果回归测试，覆盖 `self.audio_speaker.set_volume` 的实际返回形态。
3. MultiNet 启动、输入缓冲和 worker 资源失败增加可判别日志，待机唤醒初始化最多重试 3 次。
4. 唤醒命令保持为“你好牛牛”“牛来”“别说了”，阈值记录为 `0.20`；唤醒注入脚本支持 `--expected-word`。

## 实板证据

### 告别回合

原始日志：`test-artifacts/wake-sensitivity-20260826/multiturn-20260826-212428.log`

输入通过百炼 TTS 注入“再见”，设备最终识别为“再见”，回复为“牛牛走了～”。关键顺序：

```text
SERIAL_VOICE_EVIDENCE event=tts_stopped
WAKE_GUARD_ARMED ms=8000 reason=terminal_tts
VoiceLifeWake: WAKE_WORKER_ARMED generation=3
SERIAL_VOICE_EVIDENCE event=standby_ready
```

该回合结果 JSON 中 `completed=true`、`asr_matches_input=true`、`tts_first_audio_seen=true`、`tts_stopped_seen=true`，且 `in_drop=0`、`in_pool_fail=0`、`out_reject=0`、`short_write=0`、I2S 错误均为 0。守护窗观察 8.5 秒未出现误唤醒或重连。

这证明“再次唤醒困难”不是检测器没有重新启动，而是告别 TTS 结束后仍有固定 8 秒的 `SuppressLocalWakeFor` 窗口。当前记录只证明原因，尚未把保护窗缩短作为本次提交的一部分。

### 启动门禁

启动日志确认：`DISPLAY_READY=1`、`MCP_TOOLS_READY count=9`、`SPARKBOT_COMMON_FONT_ASSET_OK`、`WAKE_COMMAND_STATUS threshold_value=0.20`、`WAKE_WORKER_ARMED` 和 `standby_ready` 均出现，未出现“出错了”。

## 主机验证

```text
./scripts/run_host_tests.sh -R linx_mcp_bridge_coverage_test
1/1 Test #45: linx_mcp_bridge_coverage_test ... Passed
```

该测试覆盖布尔成功结果、结构化 `success`、`conflict`、失败和 JSON-RPC 错误分支。

## 后续百炼对照

唤醒灵敏度需要在实板上分别统计“你好牛牛”和“牛来”的命中率、不同语速/声调和无关键词负样本的误触发率；在取得这组数据前，不把继续降低 MultiNet 阈值宣称为修复。音量工具需在百炼对话中验证 0、30、70、100 四个边界值，并确认串口只出现成功结果，不再先显示“操作错误”。

参考：

- https://linx.qiniu.com/docs/xrobot/platform/websocket
- https://linx.qiniu.com/docs/xrobot/mcp/hardware-mcp
- https://github.com/78/xiaozhi-esp32
