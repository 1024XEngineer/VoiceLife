# ADR 0003：限定 MVP 周期任务为 UTC

- 状态：Accepted
- 日期：2026-08-07
- 决策人：VoiceLife 团队
- 关联：Issue #195、MS3

## 决策

MVP 的周期任务只支持 `UTC`。`start_at`、`next_trigger_at`、`effective_until` 和 Calendar 查询范围均为 Unix 秒时间戳；周期的日、周、月、年计算使用 UTC 公历。单次任务仍可携带 `Asia/Shanghai` 等时区字符串，因为其触发时刻已经是确定的绝对时间，不参与周期展开。

不引入 IANA tzdb、固定 `Asia/Shanghai` 规则或 `TimeZonePort`。到期推进、重启恢复和 Calendar 展开必须使用同一套 UTC 周期规则，不能各自解释时区或把不支持的时区静默当成 UTC。

## 公开契约和错误语义

- `TimingTaskService::RegisterTimerTask` 收到 `recurrence.frequency != kNone` 且 `time_zone != "UTC"` 时，返回 `kInvalidArgument`，不创建任务或提醒规则。
- 对已有非 UTC 单次任务，`UpdateTimerTask` 试图设置非空周期规则时，同样返回 `kInvalidArgument`。
- 已持久化的非 UTC 周期任务保留为兼容窗口内的历史数据；`ListCalendarView` 返回 `kUnavailable`，而不是按 UTC 或设备本地时区推算。到期推进和恢复接口落地时必须采取相同的拒绝语义。
- `time_zone == "UTC"` 的日、周、月、年周期规则按既有字段校验；无效筛选字段或取值仍返回 `kInvalidArgument`。

这使“允许注册”与“系统能可靠执行”保持一致。当前错误信息为 `MVP 周期任务仅支持 UTC 时区`；调用方应以错误码处理，不能依赖中文文本。

## 周期边界

- **锚点**：`start_at` 是唯一锚点，保留其 UTC 日期和当天时间。候选 occurrence 不早于 `start_at`。
- **日**：每个 UTC 日在锚点的当天时间生成一次。
- **周**：未指定 `by_weekdays` 时，每七天从锚点生成一次；指定时按 UTC 周一为 `1` 至周日为 `7` 过滤。
- **月**：未指定 `by_month_days` 时使用锚点的月日；指定时使用这些月日。目标月不存在该日期时跳过，不向该月最后一天或下月补偿。例如 1 月 31 日规则在平年 2 月不生成 occurrence。
- **年**：未指定筛选时使用锚点月日；指定 `by_months`、`by_month_days` 时取两者交集。2 月 29 日仅在闰年生成，不回退到 2 月 28 日或 3 月 1 日。
- **有效期**：`effective_until == 0` 表示没有上界；否则仅生成满足 `candidate < effective_until` 的 occurrence。future 取消把 `effective_until` 设为 `effective_from`，即第一个被排除的 occurrence 时刻。
- **查询窗口**：Calendar、到期推进和恢复均使用 `[range_start, range_end)`；起点包含，终点不包含。

## 方案取舍

| 方案 | 结论 | 原因 |
| --- | --- | --- |
| UTC-only | 采用 | 当前 UTC 公历实现已可由纯 C++ 主机测试覆盖，且没有夏令时歧义。 |
| 固定 `Asia/Shanghai` | 不采用 | 会让协议宣称支持一个本地时区，却无法为迁移、设备配置和未来多时区给出统一能力边界。 |
| 最小时区 Port | 延后 | 它仍需定义 tzdb 来源、夏令时歧义、存储版本与恢复兼容性，超出 MVP 资源与范围。 |

## 测试矩阵

| 场景 | 公开入口 | 期望结果 | 覆盖责任 |
| --- | --- | --- | --- |
| UTC 日、周、月、年注册 | `RegisterTimerTask` | 成功，锚点与规则被保存 | Timing 主机测试 |
| 非 UTC 周期注册 | `RegisterTimerTask` | `kInvalidArgument`，不写入任务 | Timing 主机测试 |
| 非 UTC 单次改为周期 | `UpdateTimerTask` | `kInvalidArgument`，不写入更新 | Timing 主机测试 |
| 历史非 UTC 周期读取 | `ListCalendarView` | `kUnavailable`，不进行推算 | Calendar 回归测试 |
| 查询端点 | `ListCalendarView` | 仅返回 `[range_start, range_end)` 内 occurrence | Calendar 回归测试 |
| 月末 | Calendar、到期推进、恢复 | 不存在的日期跳过，不夹逼到月末 | 三个消费者共用的契约测试 |
| 闰年 | Calendar、到期推进、恢复 | 2 月 29 日仅在闰年生成 | 三个消费者共用的契约测试 |
| `effective_until` | Calendar、到期推进、恢复 | `candidate < effective_until`，future 边界不产生 occurrence | Timing 主机测试与消费者契约测试 |

Calendar 已通过 `TimingTaskService` 主机测试覆盖注册拒绝、更新拒绝、UTC 展开和左闭右开有效期。到期推进与恢复接口尚未实现；实现它们的 Issue 必须将月末、闰年、历史任务和有效期行纳入同一套契约测试，不能复制另一套日期算法。

## 兼容与回退

本决策不迁移或删除旧任务。历史非 UTC 周期任务保持可读但不可展开，调用方收到 `kUnavailable` 后应提示重新以 UTC 创建或等待后续时区能力。回退本变更时，可恢复旧版本的注册行为，但这会重新允许无法可靠展开的任务；因此只可作为紧急版本回退，不能作为长期兼容策略。
