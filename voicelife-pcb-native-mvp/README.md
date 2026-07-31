# VoiceLife PCB Native MVP

这个目录是 PCB 小智全板载迁移的独立工作区。VoiceLife C++ MVP 已编译并刷入真机验证；Mac 端原型和 #62 副本没有改动。当前板子保留在 MVP 固件，原始整片 Flash 仍可按备份恢复。

## 当前内容

- `backup/esp32s3-original-flash-20260729.bin`：设备整片 16 MB Flash 原始镜像，本地保留，因包含设备身份信息不纳入 PR
- `backup/restore.md`：恢复命令和安全边界
- `docs/native-cpp-migration-plan.md`：#62 行为迁移到 ESP32 C++ 的产品与架构方案
- `docs/mvp-comprehensive-test-plan-20260730.md`：全面测试结果、演示脚本和未通过项
- `docs/linx-agent-prompt-final.txt`：已同步到灵矽 Agent 的当前 Prompt
- `test-evidence/20260729-hardware-reminder/serial-final-v11.log`：真机提醒、Linx WebSocket、TTS 和音频包证据
- `test-evidence/20260729-hardware-reminder/serial-after-v11-reboot.log`：重启后的持久化与防重复播报证据
- `test-evidence/20260729-hardware-reminder/voicelife-after-v11-first64k.bin`：提醒状态回读（`pushed`），本地保留，因包含板载状态不纳入 PR

设备识别结果：ESP32-S3，16 MB Flash，8 MB PSRAM，40 MHz 晶振。

## MVP 验证结果（2026-07-29）

- 本地 VoiceLife 存储加载日程和提醒，提醒到期后状态写回 `pushed`。
- 设备通过 `ws://xrobo-io.qiniuapi.com/v1/ws/` 建立 Linx 会话，主动播报文本被接受。
- 串口出现 `TTS started`、`received TTS audio packet`、`TTS stopped`，音频输出链路随后自动关闭。
- 真机重启后仍加载提醒，但没有再次出现 `Delivering`，确认不会重复播报。
- PCB 与 PR #85 Gateway 的 IM 同步契约已实现并通过自动测试；真实公众号投递仍未配置 live 微信凭据，继续按低优先级处理。
- 灵矽 PCM 文件流已验证 ASR、工具调用和 TTS。点提醒可演示；未给时长的会议会被模型擅自补成 60 分钟，暂时不要用于演示。

整片镜像 SHA-256：

```text
4e3ea1bd77873dc2b300f7b14adf0c3b5b93ceb15a8febe15d1c19464b76385d
```

备份文件包含 NVS、Wi-Fi 配置和设备身份信息，不上传到公共仓库或聊天工具。本 PR 只提交备份清单、恢复说明和哈希记录，不提交板载 `.bin` 镜像。
