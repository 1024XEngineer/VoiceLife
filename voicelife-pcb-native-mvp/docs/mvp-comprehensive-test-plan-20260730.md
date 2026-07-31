# VoiceLife PCB MVP 全面测试与演示结论（2026-07-30）

结论：点提醒 MVP 已具备演示条件，但不是全绿。PCB 日程核心 31/31、#62 回归 48/48、PR #85 Gateway 42/42、真机自动播报和恢复均通过；灵矽 PCM 多场景功能通过 7/8，剩余功能缺口是模型会在用户未给时长时擅自把时间段补成 60 分钟。

下一步：演示只使用“提醒我”这类 point 日程，时间段必须把开始和结束时间都说全。不要再手动重填同一份 Prompt；当前 Prompt 已同步，剩余的多余播报是 `deepseek-v4-flash` 在灵矽工具链中的执行不稳定，不是控制台漏配。

## 1. 当前验收判定

| 验收门 | 结果 | 证据 |
| --- | --- | --- |
| PCB C++ 日程、提醒、存储 | PASS，31/31 | `firmware/scripts/run_voicelife_host_tests.sh` |
| 灵矽 PCM 单次创建严格回复 | PASS | `test-evidence/20260730-linx-agent-no-narration-pcm/manifest.json` |
| 灵矽 PCM 多场景功能 | PARTIAL，7/8 | `test-evidence/20260730-linx-pcm-comprehensive/manifest.json` |
| 灵矽回复整洁度 | FAIL，严格通过 4/8 | 同上；存在前置说明、Prompt 原句外泄 |
| 真机到期自动播报 | PASS | `test-evidence/20260730-comprehensive-hardware-auto-broadcast/manifest.json` |
| 真机重启持久化、防重复播报 | PASS | `test-evidence/20260729-hardware-reminder/` |
| Flash 测试后恢复 | PASS | 仅操作 `voicelife` 分区，测试后恢复；未整片擦除 |
| #62 Mac 原型回归 | PASS，48/48 + typecheck | `XE6-15-pr62-fresh`，工作树保持干净 |
| PR #85 Gateway | PASS，42/42 | `XE6-15-pr85-review/demos/wechat-xiaozhi` |
| 真实微信公众号投递 | NOT RUN | 缺 live 微信凭据；IM 仍是低优先级 |
| 物理麦克风/唤醒词完整实录 | NOT RUN | PCM 文件流已通过，但不能替代板载麦克风声学验证 |

因此本轮判定是：**点提醒演示可用，完整日程助手暂未过线。**

## 2. 测试边界

本轮用了三层证据，三者不能混为一个测试：

1. **灵矽 PCM 文件流**：真实经过 OTA、WebSocket、ASR、Agent、MCP tool call 和 TTS；MCP 返回是确定性测试夹具，不写 PCB。
2. **PCB C++ host 测试**：运行板端同一套 `VoiceLifeService` 业务代码，覆盖日程、冲突、周期、提醒、持久化和 IM 协议；不经过云端语音模型。
3. **真实 PCB hardware smoke**：在 `voicelife` 分区注入未来到期夹具，确认板子连接灵矽、收到 TTS 音频、播放并写回 `pushed`，然后恢复测试前分区。

这三层已经证明各段能工作，但尚未留下一条“物理麦克风说话 -> 真机创建 -> 同一事项到期播报”的连续串口证据。演示前应补这一条，届时不需要重刷固件。

## 3. 灵矽 PCM 用例

| ID | 输入 | 关键验收 | 功能 | 回复质量 |
| --- | --- | --- | --- | --- |
| PCM-01 | 明天上午九点提醒我开会 | `calendar_create`、`kind=point`、时间正确、TTS 等于 `speech` | PASS | PASS，独立严格测试连续两次通过 |
| PCM-02 | 查询我明天的日程 | `calendar_query`，范围为自然日左闭右开，播报两条 | PASS | PASS |
| PCM-03 | 把明天九点的开会改到十点 | `calendar_find -> calendar_modify`，候选和新时间正确 | PASS | 本次 PASS，历史轮次出现“我先查一下” |
| PCM-04 | 创建一个与已有会议冲突的提醒 | 首次不带令牌，说明冲突并等待确认，不假报成功 | PASS | PASS |
| PCM-05 | 记一下门禁卡的位置 | `note_record`，内容正确 | PASS | FAIL，偶发朗读工具选择理由 |
| PCM-06 | 每个工作日上午九点提醒 | 不调用工具，不把工作日近似成 weekly | PASS | PASS，但会追加替代建议 |
| PCM-07 | 明天下午三点安排评审会 | 缺结束时间时不调用工具，只追问时长 | **FAIL** | 模型擅自填 `durationMinutes=60`，并重复调用 |
| PCM-08 | 已过去的钟点提醒 | 不创建过去事项，询问今天还是明天 | PASS | FAIL，会先朗读当前时间和推导 |
| PCM-09 | 保存银行卡密码 | 不调用工具，拒绝保存 | PASS | FAIL，偶发逐字朗读 Prompt 规则 |

PCM-07 是本轮唯一功能阻塞。Prompt 和工具规则都明确禁止猜时长，模型仍会随机补 60 分钟，因此不能把它写成“已修复”。

## 4. PCB C++ 用例

以下 31 个测试在 2026-07-30 重新编译并运行，全部通过。

### 日程、提醒和存储（21）

| ID | 覆盖内容 | 结果 |
| --- | --- | --- |
| CPP-01 | ISO-8601：`Z`、偏移、小数秒、非法日期 | PASS |
| CPP-02 | 查询边界、跨边界时间段、find 参数校验 | PASS |
| CPP-03 | 闰日、月末、31 号跳过短月 | PASS |
| CPP-04 | 创建、查询、查找、修改、删除、撤销 | PASS |
| CPP-05 | 空参数、非法时间、冲突、相邻不冲突 | PASS |
| CPP-06 | 创建/修改确认令牌与原参数绑定 | PASS |
| CPP-07 | #62 创建契约：过去时间、提醒时间、point/time_block | PASS |
| CPP-08 | daily/weekly/monthly 展开与重复送达 | PASS |
| CPP-09 | 跳过单个周期实例并撤销 | PASS |
| CPP-10 | 单次日程跳过确认和幂等 | PASS |
| CPP-11 | 主提醒偏移、弱提醒和撤销 | PASS |
| CPP-12 | 存储提交失败回滚 | PASS |
| CPP-13 | 时间段默认弱提醒 | PASS |
| CPP-14 | 提醒关闭、详情、最多三次推迟 | PASS |
| CPP-15 | 推迟 1/1440 分钟边界和已关闭状态 | PASS |
| CPP-16 | 临时记录、敏感内容拒绝、24 小时过期 | PASS |
| CPP-17 | 撤销快照和重启恢复 | PASS |
| CPP-18 | 撤销过期、错误确认令牌 | PASS |
| CPP-19 | 损坏 journal 从空状态恢复 | PASS |
| CPP-20 | 音频失败重试和多提醒批处理 | PASS |
| CPP-21 | 周期暂停、恢复、永久终止 | PASS |

### PCB 与 IM Gateway 契约（10）

| ID | 覆盖内容 | 结果 |
| --- | --- | --- |
| IMC-01 | 到期重试、持久化回执、重复抑制 | PASS |
| IMC-02 | Gateway 业务拒绝后保持可重试 | PASS |
| IMC-03 | 关闭动作、ACK 和重复回放 | PASS |
| IMC-04 | 本地语音 Tick 前完成推迟 | PASS |
| IMC-05 | ACK 丢失和重启后的幂等 | PASS |
| IMC-06 | 非法或未知动作 | PASS |
| IMC-07 | HTTP 状态、JSON 和响应大小边界 | PASS |
| IMC-08 | 推迟分钟边界、URL 编码 | PASS |
| IMC-09 | 已关闭提醒拒绝后续推迟 | PASS |
| IMC-10 | 多条到期提醒独立上报 | PASS |

## 5. 真机用例

| ID | 场景 | 验收 | 结果 |
| --- | --- | --- | --- |
| HW-01 | USB/芯片识别 | ESP32-S3、16 MB Flash、8 MB PSRAM | PASS |
| HW-02 | 原始 Flash 备份 | 16 MB 镜像和 SHA-256 可校验 | PASS |
| HW-03 | 只操作业务分区 | 仅 `0xE00000..0xFFFFFF` 的 `voicelife` 分区 | PASS |
| HW-04 | 未来提醒到期 | 日志出现 `Delivering` | PASS |
| HW-05 | 灵矽主动 TTS | 握手、TTS start、音频包、stop | PASS |
| HW-06 | 状态写回 | 提醒变为 `pushed` | PASS |
| HW-07 | 重启防重复 | 重启加载状态但不重复播报 | PASS |
| HW-08 | 测试后恢复 | 分区恢复成功，无 `erase_flash` | PASS |
| HW-09 | 板载麦克风连续链路 | 唤醒、ASR、创建、到期播放同一事项 | NOT RUN |
| HW-10 | 听感/音量 | 现场确认可听清、无爆音 | NOT RUN |

## 6. #62 与 Gateway 回归

### #62 Mac 原型

`XE6-15-pr62-fresh` 没有被本轮修改。9 个测试文件、48 个测试和 TypeScript typecheck 全部通过，作用是确认板端迁移没有反向破坏原型基线。

### PR #85 Gateway

42 个测试全部通过，覆盖鉴权、请求大小、绑定码、微信加解密、H5 操作令牌、重复点击、租约、ACK、跨设备隔离和“Gateway 不得创建云端日程”。

尚未验证真实公众号发信，因为没有 live 微信凭据。这个缺口不阻塞当前语音提醒演示。

## 7. 演示脚本

演示前先看当前时间，然后只跑这三步：

1. 说：“你好小智，五分钟后提醒我喝水。”
2. 说：“查询我今天接下来的日程。”
3. 等待到期，确认 PCB 自动播报“喝水”提醒。

要演示时间段，必须说完整：“明天下午三点到四点安排项目评审会。”不要只说“明天下午三点安排项目评审会”，模型可能擅自补 60 分钟。

演示时暂时避开：工作日周期、未给时长的会议、敏感记录，以及需要非常干净话术的多步修改。它们不是全部失效，而是当前灵矽模型的输出稳定性不够。

## 8. 已知问题与修复位置

### P0：未给时长时模型猜 60 分钟

- 现象：`calendar_create` 被调用两次，参数包含用户没有说过的 `durationMinutes=60`。
- 影响：会创建错误时长的事项。
- 当前处理：演示要求说出结束时间；测试保留为 FAIL。
- 正确修复位置：灵矽模型/工具调用策略，或在工具契约中增加可验证的“用户明确提供时长”来源。单靠 Prompt 已证实不可靠。

### P1：前置文本直接进入 TTS

- 现象：模型会说“我先查一下”、当前时间、日期换算或工具选择理由。
- 影响：功能通常成功，但体验不稳定，也可能外泄 Prompt 原句。
- 已排除：`thinking` 已关闭；`reasoning_effort=low`；把温度从 0.7 临时降到 0.0 后仍失败，现已恢复 0.7。
- 正确修复位置：换一个工具调用约束更稳定的模型，或由灵矽在平台层抑制 tool call 前文本，并让成功写操作直接使用 MCP `speech`。

板端无法可靠修这个问题，因为收到的是灵矽已经生成的 TTS 音频，文字在到达 PCB 前就已经变成语音流。

## 9. 配置与安全

- 当前 Agent：`VoiceLife PCB MVP`。
- 16 个真实 PCB 工具名与 Prompt 一致，没有 `voicelife.*` 旧前缀。
- Prompt 本地与远端 SHA-256：`5c1e75fe0e6fbe91d2858e52df7e9e26a4bfc6e6ebf5d2cbb5323dd8f393c53b`。
- Agent 温度已恢复为原值 `0.7`；模型、音色、设备绑定、对话历史和插件未改。
- API Key、OTA Token、Wi-Fi 密码均未写入测试证据。
- Agent 更新前备份保存在 `backup/linx-agent/`，权限为仅当前用户可读写。

## 10. 下一步优先级

1. **演示前 P0**：补一条物理麦克风创建 point 提醒并在同一事项到期时抓串口证据。
2. **演示前 P0**：固定使用上面的三步脚本，不测试缺时长会议。
3. **演示后 P1**：在灵矽控制台试用工具调用更稳定的模型，再复跑 8 个 PCM 场景；严格回复至少连续三轮全过才算修复。
4. **低优先级**：有真实公众号凭据后补 PR #85 live 投递；当前 42 个 Gateway 自动测试已足够证明协议实现。
