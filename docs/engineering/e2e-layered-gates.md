# E2E 分层门禁与真机 HIL

本文是 Host E2E、ESP32-S3 HIL 和发布验收的共同入口。工作流只负责编排；旅程语义、断言和 evidence schema 由 `scripts/run_e2e.py` 与 adapter 维护。

## 测试层级

| 层级 | 运行位置 | 入口 | 作用 | 门禁策略 |
| --- | --- | --- | --- | --- |
| unit | 主机 | `./scripts/run_checks.sh` | 领域、契约和适配器边界 | PR required |
| integration | 主机 + Postgres | Gateway `pnpm run ci` | 服务边界、持久化和 SSE | PR required |
| Host E2E | GitHub-hosted runner | `python3 scripts/run_e2e.py --layer host ...` | 真实 Gateway 进程和最小强提醒旅程 | PR required，硬超时，retries=0 |
| HIL smoke | 受控 self-hosted + 一台设备 | `--layer hil --journey im-pairing` | 烧录、配网、就绪和配对主链路 | manual，非 required |
| HIL E2E | 受控 self-hosted + 设备池 | 同一 runner CLI，按 profile 矩阵 | SparkBot/PCB 真实串口旅程 | 稳定门槛达成后再升级 |
| 手工验收 | 真实平台和物理场景 | [release checklist](release-checklist.md) | 微信、声学、显示、掉电和真实 Linx/ASR/TTS | 发布阻断，不由 Host/HIL 替代 |

## 本地命令

Host E2E 需要 Node 24、pnpm lockfile 依赖和本地 Postgres：

```bash
pnpm --dir services/im-gateway install --frozen-lockfile
DATABASE_URL=postgres://voicelife:voicelife@127.0.0.1:5432/voicelife \
  python3 scripts/run_e2e.py --layer host \
    --journey im-gateway-strong-reminder --profile host \
    --artifact-dir artifacts/host-e2e --timeout 60 --retries 0
python3 scripts/check_e2e_artifacts.py artifacts/host-e2e
python3 scripts/render_e2e_summary.py artifacts/host-e2e
```

HIL 只能在已批准的设备和私有 Gateway 上运行。设备描述文件只含 `name`、`port`、`profile`，放在设备池的受控目录，不提交仓库：

```bash
python3 -m pip install pyserial esptool
python3 scripts/run_e2e.py --layer hil --journey im-pairing \
  --profile sparkbot --artifact-dir artifacts/hil-sparkbot \
  --timeout 900 --retries 0 \
  --device "$VOICELIFE_HIL_DEVICE_ROOT/sparkbot.json" \
  --lease-dir "$VOICELIFE_HIL_LEASE_ROOT" \
  --server "$VOICELIFE_HIL_SERVER" \
  --server-dir "$VOICELIFE_HIL_SERVER_DIR" \
  --gateway-origin "$VOICELIFE_HIL_GATEWAY_ORIGIN" \
  --user-id "$VOICELIFE_HIL_USER_ID"
python3 scripts/check_e2e_artifacts.py artifacts/hil-sparkbot
```

SparkBot 真实语音 HIL 使用专用 `esp32s3-esp-sparkbot-serial-voice` 测试 Profile。它需要受控环境中的 `DASHSCOPE_API_KEY`、`dashscope` Python 包和 `ffmpeg`；API Key 不进入命令行参数或 evidence：

```bash
DASHSCOPE_API_KEY="$DASHSCOPE_API_KEY" \
python3 scripts/run_e2e.py --layer hil --journey voice \
  --profile sparkbot --artifact-dir artifacts/hil-voice \
  --timeout 900 --retries 0 \
  --device "$VOICELIFE_HIL_DEVICE_ROOT/sparkbot.json" \
  --lease-dir "$VOICELIFE_HIL_LEASE_ROOT" \
  --server "$VOICELIFE_HIL_SERVER" \
  --server-dir "$VOICELIFE_HIL_SERVER_DIR" \
  --gateway-origin "$VOICELIFE_HIL_GATEWAY_ORIGIN" \
  --user-id "$VOICELIFE_HIL_USER_ID" \
  --tts-model qwen-audio-3.0-tts-flash \
  --voice longanlingxi \
  --text '你好牛牛，请介绍一下你自己。' \
  --text '把刚才的回答再简短一点。'
python3 scripts/check_e2e_artifacts.py artifacts/hil-voice
```

`voice` journey 会先完成 application-only flash、临时设备注册、USB 配网和 readiness，再调用现有 `voice_linx_serial_multiturn_test.py` 将 DashScope TTS 结果转为 16 kHz mono S16LE PCM 注入真实设备。它只允许 SparkBot；PCB 没有 `CONFIG_VOICELIFE_SERIAL_VOICE_TEST`，不得套用该命令。结果证明真实云端语音和设备数字播放链路，不替代麦克风声学、AEC 或全双工验收。

Gateway host、目录、origin 和 user id 通过环境变量或受控 secret 注入；token 不进入 shell 参数、日志或 evidence。HIL 默认 `retries=0`，只允许基础设施层在 workflow 外显式重试，并且必须在 summary 中可见。

## workflow 与设备矩阵

- `.github/workflows/ci.yml` 仅运行稳定 Host E2E，并上传 14 天的 JSON evidence。
- PR Host job 另运行一次受控的 `lifecycle-example` 故意失败，验证失败 evidence 和 `product` 分类仍能通过脱敏校验。
- `.github/workflows/hil-nightly.yml` 当前仅开放 `workflow_dispatch`，只接受 `self-hosted, voicelife-hil` 及 profile 标签；`fail-fast=false`，SparkBot 和 PCB 独立汇总、独立上传 artifact。`voice` 只允许选择 SparkBot，并需要受控 `DASHSCOPE_API_KEY`。
- 手工触发可选 `sparkbot`、`pcb` 或 `all`，并可在选择单一 Profile 后指定 device descriptor 名称，用于隔离故障设备；配置受控 Runner 后再执行。
- public GitHub-hosted Runner 不接触硬件或长期平台凭据。受控 Runner 只保存原始串口日志；公开 artifact 只包含通过 `scripts/check_e2e_artifacts.py` 校验的脱敏 JSON。
- artifact 保留 14 天，访问权限跟随仓库 Actions 权限；发现凭据或个人数据时立即删除公开 artifact，保留受控私有原始日志并按 [SECURITY.md](../../SECURITY.md) 上报。

## 失败分类与重试

每个 evidence 都带 `failure_category`、`failed_phase` 和稳定 `message_code`，job summary 按 profile 展示：

| 分类 | 例子 | 处理 |
| --- | --- | --- |
| `configuration` | 缺 descriptor、缺 pyserial、Profile 参数错误 | 修 Runner 配置，不重试 |
| `lease` | 设备或串口租约冲突 | 等待或释放租约后重跑，不归因于产品 |
| `device` | 串口打开失败、分区/Profile 不匹配 | 隔离该设备，修复或替换后手动重跑 |
| `infrastructure` | 构建工具、SSH/进程或 artifact 写入失败 | 修 Runner 基础设施；不把产品判定为失败 |
| `external` | Gateway/微信/ASR/TTS 服务不可用 | 记录外部依赖和时间窗口，允许一次可见的基础设施重试 |
| `product` | readiness 或 pairing 断言失败 | 保留 evidence，按产品缺陷处理，不自动重跑掩盖 |
| `timeout` / `cleanup` | 硬超时或资源回收失败 | 标记为失败并阻止设备继续进入池中 |

没有设备租约、串口异常、外部服务故障和产品断言必须在 summary 中保持不同类别。设备 workflow 默认 retries=0；只有 `infrastructure`/`external` 经批准后才能有限重试。

## Evidence 与新增 journey

公共 evidence 只允许 run/correlation id、时间、Profile、阶段状态、断言、数值指标和 HIL 的固件/commit/fingerprint。禁止原始串口、token、密码、SSID、用户/设备 ID、URL 凭据和任意命令输出。新增旅程按 [journey template](e2e-journey-template.md) 补齐：准备/运行/断言/采集/清理、失败类别、清理动作、超时预算、脱敏字段、Host/HIL 入口和一条故意失败测试。

## 稳定门槛

HIL 连续 14 次手工执行、每个 Profile 至少 10 次通过，且没有未分类失败、泄露扫描失败或设备租约泄露，才能提议升级为 required check。任意设备连续两次 `device`/`infrastructure` 失败即隔离并暂停该 Profile；不得通过增加 retries 把红灯隐藏。

真实微信公众号、H5 推迟、SSE 重连、声学、显示、物理输入和掉电验收仍见 [release checklist](release-checklist.md) 与 [Issue #132](https://github.com/1024XEngineer/VoiceLife/issues/132)。DashScope PCM 注入只能作为真实语音数字链路证据，不能替代这些声学和体验验收。
