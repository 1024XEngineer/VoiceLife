# 语音日程边界调研与本期取舍

结论：行业里的“日程边界”不是把所有自然语言都交给模型猜，而是把时间、周期、例外、时区和操作范围拆成可以确认的领域对象。VoiceLife 当前应继续承诺固定 UTC+8 下的单次、每日/每周/每月固定日或月末、每年固定月日，以及单次 skip/modify；其余语义必须澄清或明确拒绝。

动作：将本报告作为语音日程联调矩阵的边界依据。测试先验证业务事实和操作范围，再验证 STT、状态机、播报和屏幕；发现查询展开或例外应用错误时归日程模块，不在语音层加句子特判。

## 1. 调研范围

本轮核对了三类资料：

1. 日历领域标准和产品 API：RFC 5545、Google Calendar Recurring Events、Microsoft Graph `recurrencePattern` / `recurrenceRange`。
2. 语音交互实现：OpenAI Realtime 的 VAD/取消与音频截断、LiveKit Agents 的 turn/interruption 处理、Home Assistant Assist 的分阶段事件。
3. 本地实现：VoiceLife 的 recurrence planner、`ScheduleRuleService`、`schedule.query` MCP 编排、小智音频/状态机源码。

本轮环境没有可用的 WebSearch/WebFetch MCP，因此外部资料通过官方文档直连获取；每个结论都保留原始 URL，代码结论以当前分支源码为准。

## 2. 行业共同边界

| 领域 | 行业做法 | 对 VoiceLife 的含义 |
| --- | --- | --- |
| 一次性时间 | 缺少日期或时间时追问；相对日期按设备/账户时区解释 | “明天”“下午”不能直接写库，必须补齐最小缺失字段 |
| 输入格式 | 日历 API 在领域边界拒绝非法 civil date/time，而不是让解析器把字符串“尽量读完” | `YYYY-MM-DD HH:mm:ss`、`YYYY-MM-DD`、`HH:mm:ss` 必须完整匹配；不存在日期（如平年 2 月 29 日）和尾随垃圾都不能写入 |
| 周期模式 | 频率和范围分开：daily/weekly/absolute monthly/relative monthly/yearly；范围可为结束日期、次数或不结束 | 当前只承诺 daily、weekly、monthly specific/last_day、yearly；`occurrence_count` 仍拒绝 |
| 无效发生点 | RFC 5545 要求无效日期或不存在的本地时间忽略，且不计入 occurrence count | 每月 31 日遇到短月、每年 2 月 29 日遇到平年都应跳过，不得静默改成月末 |
| 单次例外 | 用“原始发生时间”定位实例；skip/modify 只影响一次 | 未来未物化实例使用 `rule_id + original_start_time`，已物化实例使用 `schedule_id` |
| 系列范围 | “本次”“本次及以后”“整个系列”是不同操作；Google 修改后续实例时会拆成两个系列 | 当前没有“本次及以后”接口，语音必须澄清为本次或整条规则，不得猜测 |
| 相对周期 | 第 N 个周几、最后一个工作日属于标准能力，但不是固定日期的别名 | 当前不支持时明确告知限制，不能降级为某个固定日期 |
| 时区 | 标准和 Google/Microsoft 都把时区作为规则语义的一部分；夏令时会改变 UTC 映射 | 当前固定 UTC+8、无 DST；用户说纽约/夏令时必须拒绝或要求改为北京时间 |
| 查询 | 查询窗口应展开窗口内所有 occurrence，并合并例外后的有效结果 | 不能用固定几条“最近发生”代替远期查询 |
| 提醒 | 提醒是独立的主动播报生命周期，有排队、去重、取消、过期和与当前对话的抢占关系 | Reminder 不等同于当前 Turn 的 TTS；必须单独记录 announcement 状态 |
| 全天/跨日 | Google Calendar、EventKit 等把全天事件和有时长事件分开；结束时间可以跨本地日期 | 当前没有 `all_day` 或跨午夜语义；“周六全天”“23 点到次日 1 点”必须澄清或拒绝，不能压成同一天的非法结束时间 |
| 提前提醒 | 常见产品把事件时间和提醒偏移分开，支持多个提醒或不同投递渠道 | 当前只在开始时播报，未开放“提前 10 分钟/多个提醒”；语音不得把“提前”静默当作准点提醒 |
| 参与者/忙闲 | Google/Microsoft 将参与者、响应状态、会议资源和 busy/free 与事件本体分开 | 当前无邀请、参与者、忙闲冲突协商；“叫小王参加”必须说明暂不支持，不能伪造已通知 |
| 语言和地区 | “下周”“工作日”“下午”依赖地区、周起始日和用户习惯，产品通常会回读解析结果请求确认 | 当前固定 UTC+8 且没有 locale 配置；相对日期、模糊时段或多个候选对象必须澄清，不能由模型凭概率落库 |

## 3. 与当前代码的逐项核对

### 已经与边界一致的部分

- `recurrence_planner.cc:118-149` 对每月指定日期会跳过不存在的日期，对每年 2 月 29 日会跳过非闰年；这符合 RFC 5545 的“忽略且不计数”规则。
- `schedule_rule_service.cc:265-277` 将未来未物化实例和已物化实例分开处理，使用例外记录保存 skip/modify；定位字段与 Google 的 `originalStartTime` 语义一致。
- `schedule_rule_service_helpers.cc:41-44` 拒绝 `occurrence_count`，因此对外文档和语音回复都不能承诺“重复十次后停止”。
- `recurrence_planner.cc:14-35` 明确固定东八区偏移，没有伪装成通用时区实现。
- MCP 时间边界现在严格校验完整格式、真实日历日期和尾随字符；这类输入错误应在工具边界返回失败，不应进入语音模型重试循环或写库。

### 必须作为日程缺陷验证的部分

1. `schedule_rule_service.cc:102-123` 调用 `NextOccurrences(rule, now, 3)`，每条规则最多返回 3 个未来点；当用户查询几个月后的日期时，MCP 层会在没有候选的情况下返回空。
2. `schedule_mcp_tools.cc:291-305` 直接把 `view.upcoming_occurrences` 转成 `future_occurrences`，没有先应用 skip/modify 例外；查询可能播报已经跳过的原始时间，也可能漏掉修改后的时间。
3. 文档契约把 `occurrence_count` 列为字段，但 `schedule_rule_service_helpers.cc:41-44` 会拒绝它；这是“协议字段存在、能力未开放”的边界，必须在模型提示和测试中明确为不支持，而不是让模型反复重试。

### 必须作为语音/状态机缺陷验证的部分

- 未提供唯一日期、时间或对象时，助手要进入澄清状态，不得调用写工具。
- 工具调用成功后才播报成功；工具失败、冲突或不支持时，屏幕不能短暂显示“已创建”。
- 播放中的提醒与当前对话要有明确抢占策略：停止旧播放、保留或丢弃未听到文本的规则必须一致，不能重复播报。
- 所有迟到的 STT/TTS/工具回调必须受 turn generation 约束，不能把上一轮结果显示到新一轮屏幕上。

## 4. 本期明确承诺和不做清单

### 承诺

- 一次性事件：完整日期、开始时间，可选结束时间、地点和备注。
- 周期事件：每日、每周指定星期（含工作日位图）、每月指定日期、每月最后一天、每年指定月日；可设置间隔和结束日期。
- 例外：未来未物化实例的单次 skip/modify；已物化实例的单次更新/取消；整条周期规则取消或更新。
- 查询：设备 UTC+8 时间范围内的单次事件、周期 occurrence 和已应用例外；实现前必须修复固定三条和例外未应用问题。

### 不做

- IANA 时区、夏令时、跨时区旅行后的本地时间保持。
- 每月第 N 个周几、最后一个工作日、按工作日偏移等相对周期。
- `COUNT`/发生次数上限、任意 RRULE、依赖节假日的规则。
- “本次及以后”系列拆分；在没有明确接口前不允许模型自行选择影响范围。
- 通过语音层特殊分支把不支持的规则转换成看似相近的规则。

## 5. 边界测试新增项

以下测试补充到现有矩阵，输入仍必须使用百炼 TTS 生成真实音频，经串口注入实板：

| 用例 | 语音输入 | 预期 | 根因归属 |
| --- | --- | --- | --- |
| B1 | “每月 31 号报销”后查询 2 月和 3 月 | 2 月无 occurrence，3 月恢复 31 号；不改成 2 月末 | 日程 |
| B2 | “每年 2 月 29 号生日”后查询平年和闰年 | 平年跳过且不计次数，闰年出现 | 日程 |
| B3 | “每月第二个周二” | 明确不支持，不写入 | 语音/日程边界 |
| B4 | “从下个月开始以后都改到 10 点” | 追问影响范围；当前版本不执行系列拆分 | 语音 |
| B5 | 创建半年后的周期事件，再问“那天有什么” | 返回目标窗口内 occurrence，而不是空 | 日程查询 |
| B6 | 跳过一个未来 occurrence 后立即查询原日期 | 原 occurrence 不出现，例外可审计 | 日程查询 |
| B7 | 修改一个未来 occurrence 后查询新旧时间 | 只出现新时间，原时间不再作为 active occurrence | 日程查询 |
| B8 | “纽约时间每天 9 点”或包含夏令时描述 | 说明只支持北京时间，要求改写或拒绝 | 语音/产品边界 |
| B9 | “每周一到周五”与“每周工作日” | 两者都映射到明确 weekday mask，播报复述完整 | 语音 |
| B10 | 当前 TTS 播放时提醒到点，同时用户说“停一下” | 只保留一个可听结果，状态和 generation 单调递增 | 语音/状态机 |
| B11 | `2026-02-29`、`2026-02-28xyz`、`09:00:00extra` | 工具边界拒绝；无日程、无提醒、无“已创建”播报 | 日程/协议边界 |
| B12 | “周六全天值班”“今晚 23 点到明天 1 点” | 当前版本澄清或拒绝，不把跨日事件改成同日非法范围 | 产品/日程边界 |
| B13 | “提前十分钟提醒我”或“提前十分钟、提前一分钟各提醒一次” | 明确告知当前只支持到点提醒，不创建伪造的提前任务 | 产品/提醒边界 |
| B14 | “叫小王参加评审”“这段时间谁有空” | 不声称已邀请或已查询忙闲；说明当前能力范围 | 产品/日程边界 |
| B15 | “下周一”在周起始日切换、跨年、午夜附近输入 | 回读绝对日期后再确认；日期不确定时只追问，不写入 | 语音/本地化边界 |

## 6. 来源

1. [RFC 5545 Recurrence Rule](https://www.rfc-editor.org/rfc/rfc5545.txt)：无效日期/不存在本地时间必须忽略；`RECURRENCE-ID` 用于实例例外；时区语义不能省略。
2. [Google Calendar Recurring Events](https://developers.google.com/calendar/api/guides/recurringevents)：实例通过 `originalStartTime` 唯一定位；单次修改产生 exception；修改“本次及以后”需要拆分系列。
3. [Microsoft Graph recurrencePattern](https://learn.microsoft.com/en-us/graph/api/resources/recurrencepattern)：支持 absolute/relative monthly 和 yearly 模式，频率与间隔独立。
4. [Microsoft Graph recurrenceRange](https://learn.microsoft.com/en-us/graph/api/resources/recurrencerange)：范围独立支持 `endDate`、`noEnd` 和 `numbered`。
5. [OpenAI Realtime OpenAPI](https://github.com/openai/openai-openapi)：server VAD、`response.cancel` 和按实际播放位置 `conversation.item.truncate`，说明打断必须同步媒体和上下文。
6. [LiveKit turn handling](https://docs.livekit.io/agents/build/turns/)：VAD、语义 turn detector、假打断和播放历史截断分层处理。
7. 本地 [schedule-voice-integration-test-matrix.md](schedule-voice-integration-test-matrix.md)、[recurrence_planner.cc](../../components/voicelife_schedule/src/rules/recurrence_planner.cc) 和 [schedule_rule_service.cc](../../components/voicelife_schedule/src/service/schedule_rule_service.cc)：当前产品承诺与实现缺口。
8. [Google Calendar events resource](https://developers.google.com/calendar/api/v3/reference/events)：事件的 `start` / `end`、`reminders`、参与者和时区是分开的字段，不能把提醒偏移或参与者当作事件标题的一部分。
9. [Google Calendar free/busy](https://developers.google.com/calendar/api/v3/reference/freebusy/query)：忙闲查询是独立 API 和权限边界，不等于本地时间冲突检查。
10. [Apple EventKit EKEvent](https://developer.apple.com/documentation/eventkit/ekevent)：`isAllDay`、开始/结束时间和时区是独立事件语义，全天事件不是把开始时间设为午夜的普通事件。

## 7. 复查触发条件

当引入 IANA 时区、系列后续修改、发生次数、节假日工作日或主动播报优先级后，必须重新审查本报告和矩阵；新增字段不能只改 MCP schema，必须同步更新语音澄清策略、提醒排程、屏幕显示和实板回归。
