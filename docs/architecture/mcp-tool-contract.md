# MCP Tool 契约

本文定义 `components/voicelife_mcp` 暴露给语音 Provider 的 MCP Tool 契约。当前只保留日程领域的四个工具，周期规则不再单独暴露为 MCP Tool，而是通过 `schedule.create`、`schedule.update`、`schedule.delete` 中的嵌套字段或目标 ID 表达。

## 工具总表

| 工具名称 | 工具类型 | 工具描述 |
| --- | --- | --- |
| `schedule.create` | `mcp.tool` | 创建一次性日程或周期日程规则 |
| `schedule.query` | `mcp.tool` | 按自然语言友好的条件查询当前相关日程 |
| `schedule.update` | `mcp.tool` | 更新日程、更新周期规则、取消或跳过某次日程 |
| `schedule.delete` | `mcp.tool` | 删除单次日程或整条周期规则 |

## 通用约定

### 发现协议

`tools/list` 返回每个工具的名称、描述和输入 Schema。所有输入字段必须包含 `description`，模型依赖这些描述理解参数语义。

### 调用协议

工具入参通过 MCP `tools/call` 的 `arguments` 传入。当前支持字符串、整数、布尔值和对象。

### 返回协议

工具执行成功时，MCP `text` content 返回一个 JSON 字符串。统一结果字段如下。

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| 全部工具 | `status` | string | 是 | 本次工具调用结果状态：`success`、`conflict`、`failure` |
| 全部工具 | `message` | string | 是 | 面向模型的结果描述，例如 `created success` |

失败返回：

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| 全部工具 | `status` | string | 是 | 固定为 `failure` |
| 全部工具 | `message` | string | 是 | 失败原因 |

冲突返回：

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| 全部工具 | `status` | string | 是 | 固定为 `conflict` |
| 全部工具 | `message` | string | 是 | 冲突描述，例如 `schedule conflict` |
| 全部工具 | `conflicts` | array | 是 | 冲突日程列表，元素为 `schedule` |

### 时间格式

| 场景 | 格式 | 示例 |
| --- | --- | --- |
| 日程实例时间 | `YYYY-MM-DD HH:mm:ss` | `2026-08-14 09:30:00` |
| 查询日期范围 | `YYYY-MM-DD` | `2026-08-14` |
| 周期规则每日时间 | `HH:mm:ss` | `09:30:00` |
| 周期规则开始/结束日期 | `YYYY-MM-DD` | `2026-08-17` |

### 日程状态

| 状态 | 说明 |
| --- | --- |
| `active` | 有效 |
| `cancelled` | 已取消或已跳过 |
| `completed` | 已完成 |

## 通用数据结构

### `schedule`

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| 全部工具 | `id` | integer | 是 | 日程 ID |
| 全部工具 | `event` | string | 是 | 日程标题或事件内容 |
| 全部工具 | `status` | string | 是 | 日程状态：`active`、`cancelled`、`completed` |
| 全部工具 | `start_time` | string \| null | 是 | 开始时间，`YYYY-MM-DD HH:mm:ss` |
| 全部工具 | `end_time` | string \| null | 是 | 结束时间，`YYYY-MM-DD HH:mm:ss` |
| 全部工具 | `location` | string \| null | 是 | 地点 |
| 全部工具 | `notes` | string \| null | 是 | 备注 |
| 全部工具 | `rule_id` | integer \| null | 是 | 所属周期规则 ID；一次性日程为 `null` |
| 全部工具 | `repeat` | object \| null | 是 | 周期规则摘要；一次性日程为 `null` |

### `repeat`

用于创建或更新周期日程。创建时 `freq_type`、`start_date`、`start_time` 必填。

| 工具名称 | 参数/返回字段 | 类型 | 必填 | 默认值 | 最小值 | 最大值 | 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `schedule.create` / `schedule.update` | `freq_type` | string | 创建时必填 | - | - | - | 周期频率：`daily`、`weekly`、`monthly`、`yearly` |
| `schedule.create` / `schedule.update` | `interval_val` | integer | 否 | `1` | `1` | - | 周期间隔 |
| `schedule.create` / `schedule.update` | `start_date` | string | 创建时必填 | - | - | - | 规则开始日期，`YYYY-MM-DD` |
| `schedule.create` / `schedule.update` | `start_time` | string | 创建时必填 | - | - | - | 每次发生时间，`HH:mm:ss` |
| `schedule.create` / `schedule.update` | `end_time` | string | 否 | - | - | - | 每次结束时间，`HH:mm:ss` |
| `schedule.create` / `schedule.update` | `end_date` | string | 否 | - | - | - | 规则结束日期，`YYYY-MM-DD` |
| `schedule.create` / `schedule.update` | `occurrence_count` | integer | 否 | - | `1` | - | 最多发生次数 |
| `schedule.create` / `schedule.update` | `weekdays_mask` | integer | 否 | - | `0` | `127` | `weekly` 模式使用的星期掩码 |
| `schedule.create` / `schedule.update` | `day_of_month` | integer | 否 | - | `1` | `31` | `monthly` 模式使用的日期 |
| `schedule.create` / `schedule.update` | `month_of_year` | integer | 否 | - | `1` | `12` | `yearly` 模式使用的月份 |
| `schedule.create` / `schedule.update` | `monthly_mode` | string | 否 | - | - | - | 月模式：`specific_day`、`last_day` |

### `rule`

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.update` / `schedule.delete` | `id` | integer | 是 | 周期规则 ID |
| `schedule.update` / `schedule.delete` | `event` | string | 是 | 周期日程标题 |
| `schedule.update` / `schedule.delete` | `status` | string | 是 | 规则状态：`active`、`cancelled` |
| `schedule.update` / `schedule.delete` | `freq_type` | string | 是 | `daily`、`weekly`、`monthly`、`yearly` |
| `schedule.update` / `schedule.delete` | `interval_val` | integer | 是 | 周期间隔 |
| `schedule.update` / `schedule.delete` | `start_date` | string | 是 | 开始日期 |
| `schedule.update` / `schedule.delete` | `start_time` | string | 是 | 每日时间 |
| `schedule.update` / `schedule.delete` | `end_time` | string \| null | 是 | 每日结束时间 |
| `schedule.update` / `schedule.delete` | `end_date` | string \| null | 是 | 结束日期 |
| `schedule.update` / `schedule.delete` | `occurrence_count` | integer \| null | 是 | 最多发生次数 |
| `schedule.update` / `schedule.delete` | `weekdays_mask` | integer \| null | 是 | 星期掩码 |
| `schedule.update` / `schedule.delete` | `day_of_month` | integer \| null | 是 | 月内日期 |
| `schedule.update` / `schedule.delete` | `month_of_year` | integer \| null | 是 | 年内月份 |
| `schedule.update` / `schedule.delete` | `monthly_mode` | string \| null | 是 | `specific_day` 或 `last_day` |

### `future_occurrence`

`future_occurrence` 表示周期规则未来会发生、但尚未物化到 `schedule` 表的候选日程。它不返回真实 `schedule_id`，后续修改或删除通过 `rule_id + original_start_time` 定位。

| 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- |
| `rule_id` | integer | 是 | 周期规则 ID |
| `original_start_time` | string | 是 | 原始发生时间，`YYYY-MM-DD HH:mm:ss` |
| `event` | string | 是 | 日程标题 |
| `status` | string | 是 | 本次未来实例状态：`active` |
| `start_time` | string | 是 | 开始时间，`YYYY-MM-DD HH:mm:ss` |
| `end_time` | string \| null | 是 | 结束时间，`YYYY-MM-DD HH:mm:ss` |
| `location` | string \| null | 是 | 地点 |
| `notes` | string \| null | 是 | 备注 |
| `repeat` | object | 是 | 周期规则摘要 |

### `exception`

`exception` 表示 `schedule_rule_exception` 实体，用于描述周期规则中某一次已经发生的修改或跳过。

| 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- |
| `id` | integer | 是 | 例外 ID |
| `rule_id` | integer | 是 | 所属周期规则 ID |
| `original_start_time` | string | 是 | 原始发生时间，`YYYY-MM-DD HH:mm:ss` |
| `type` | string | 是 | `modify` 或 `skip` |
| `schedule_id` | integer \| null | 是 | 例外已关联的日程 ID；未关联时为 `null` |
| `override_start_time` | string \| null | 是 | 覆盖后的开始时间 |
| `override_end_time` | string \| null | 是 | 覆盖后的结束时间 |
| `override_event` | string \| null | 是 | 覆盖后的日程标题 |
| `override_location` | string \| null | 是 | 覆盖后的地点 |
| `override_notes` | string \| null | 是 | 覆盖后的备注 |

## 回调内部编排

MCP Tool 回调只做四件事：

1. 把 `arguments` 解析成业务命令所需字段。
2. 按目标类型路由到 `ScheduleService` 或 `ScheduleRuleService`。
3. 组合多个业务结果，生成模型友好的统一返回结构。
4. 把业务错误映射成 `failure`，把冲突映射成 `conflict`。

回调不应直接使用 Repository。跨一次性日程与周期规则的能力统一编排在 MCP 工具层。

### 可用业务 API

`schedule::ScheduleService`：

- `create_schedule`
- `query_schedule`
- `update_schedule`
- `cancel_schedule`

`schedule::ScheduleRuleService`：

- `create_schedule_rule`
- `query_schedule_rules`
- `update_schedule_rule`
- `cancel_schedule_rule`
- `update_schedule_occurrence`
- `skip_schedule_occurrence`

`schedule::ScheduleOperationService`：

- 如需支持撤销，可由 MCP 编排层在写操作成功后调用 `record_schedule_operation`。

### `schedule.create` 回调编排

没有 `repeat`：

1. 解析一次性字段，时间从 `YYYY-MM-DD HH:mm:ss` 转为 `DateTime`。
2. 构造 `schedule::CreateScheduleCommand`。
3. 调用 `ScheduleService::create_schedule`。
4. 映射返回：
   - `result.result.ok()` 且存在 `result.result.value` -> `status: success`
   - `result.result.status.code == kConflict` -> `status: conflict`
   - 其他 -> `status: failure`
5. 成功时返回创建后的 `schedule` 和 `conflicts`。

有 `repeat`：

1. 解析 `repeat`，枚举值转成 `Frequency` / `MonthlyMode`。
2. 构造 `schedule::CreateScheduleRuleCommand`。
3. 调用 `ScheduleRuleService::create_schedule_rule`。
4. 映射返回：
   - `result.status.ok()` -> `status: success`
   - `result.status.code == kConflict` -> `status: conflict`
   - 其他 -> `status: failure`
5. 成功时返回 `rule`，并把首条已物化实例作为 `schedule` 返回。

### `schedule.query` 回调编排

`schedule.query` 是只读操作，不物化未来实例，不写 `schedule` 表。

1. 解析 `keyword`、`status`、`start_date`、`end_date`。
2. 调用 `ScheduleService::query_schedule` 查询已物化到 `schedule` 表的日程。
3. 调用 `ScheduleRuleService::query_schedule_rules` 查询周期规则、例外和未来发生时间。
4. 对周期规则使用 recurrence planner 能力，展开查询范围内的未来 occurrence。
5. 按 `start_date`、`end_date`、`keyword`、`status` 做最终过滤。
6. 汇总返回：
   - `schedules`：已物化日程。
   - `future_occurrences`：未来周期候选日程。
   - `exceptions`：`schedule_rule_exception` 实体。
7. 将完整范围、总数、三类条目和查询时间提交到 `POST /v1/im/schedule-query-results`；使用
   `schedule-query:<request_id>` 作为幂等业务事件 ID。IM 失败只影响 `im_delivery` 字段，
   不改变返回给模型的结构化结果。
8. 向模型返回结构化 JSON（`schedules`、`future_occurrences`、`exceptions`、`result_count`、
   `recent`、`im_delivery`），不再返回自然语言文本摘要。

### `schedule.update` 回调编排

1. 根据入参识别目标：
   - 一次性日程：使用 `schedule_id`。
   - 已物化周期实例：使用 `schedule_id`。
   - 未来周期实例：使用 `rule_id + original_start_time`。
2. 只更新单次日程：
   - 已物化实例调用 `ScheduleService::update_schedule`。
   - 未来周期实例调用 `ScheduleRuleService::update_schedule_occurrence`。
3. 跳过某次周期日程：
   - 已物化实例调用 `ScheduleService::cancel_schedule`。
   - 未来周期实例调用 `ScheduleRuleService::skip_schedule_occurrence`。
4. 更新整条周期规则：
   - 通过 `rule_id` 定位。
   - 使用 `repeat` 构造 `UpdateScheduleRuleCommand`。
   - 调用 `ScheduleRuleService::update_schedule_rule`。
5. 映射返回：
   - 成功返回更新后的 `schedule` 或 `rule`。
   - 冲突返回 `status: conflict` 和 `conflicts`。
   - 其他失败返回 `status: failure` 和 `message`。

### `schedule.delete` 回调编排

1. 传 `schedule_id`：
   - 先读取删除前快照。
   - 调用 `ScheduleService::cancel_schedule`。
   - 返回被取消的 `schedule`。
2. 传 `rule_id`：
   - 调用 `ScheduleRuleService::cancel_schedule_rule`。
   - 返回被取消的 `rule`。
3. 删除未来周期单次：
   - 使用 `rule_id + original_start_time`。
   - 调用 `ScheduleRuleService::skip_schedule_occurrence`。
   - 返回对应的 `exception`。

## Tool 1：`schedule.create`

### 入参

| 工具名称 | 参数 | 类型 | 必填 | 默认值 | 最小值 | 最大值 | 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `schedule.create` | `event` | string | 是 | - | - | - | 日程标题或事件内容 |
| `schedule.create` | `start_time` | string | 否 | - | - | - | 一次性日程开始时间，`YYYY-MM-DD HH:mm:ss` |
| `schedule.create` | `end_time` | string | 否 | - | - | - | 一次性日程结束时间，`YYYY-MM-DD HH:mm:ss` |
| `schedule.create` | `location` | string | 否 | - | - | - | 日程地点 |
| `schedule.create` | `notes` | string | 否 | - | - | - | 日程备注 |
| `schedule.create` | `ignore_conflict` | boolean | 否 | `false` | - | - | 是否忽略时间冲突；为 `true` 时直接创建并返回创建后的日程 |
| `schedule.create` | `repeat` | object | 否 | - | - | - | 周期规则；不传时创建一次性日程，传入时创建周期日程 |

`repeat` 字段定义见「通用数据结构 > `repeat`」。

### 出参

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.create` | `status` | string | 是 | `success`、`conflict` 或 `failure` |
| `schedule.create` | `message` | string | 是 | 结果描述 |
| `schedule.create` | `schedule` | object \| null | 是 | 创建成功时返回 `schedule`；冲突未忽略或失败时为 `null` |
| `schedule.create` | `conflicts` | array | 是 | 冲突日程列表；无冲突时为空数组 |

## Tool 2：`schedule.query`

### 入参

| 工具名称 | 参数 | 类型 | 必填 | 默认值 | 最小值 | 最大值 | 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `schedule.query` | `keyword` | string | 否 | - | - | - | 按日程标题或备注模糊搜索 |
| `schedule.query` | `status` | string | 否 | `active` | - | - | 日程状态筛选：`all`、`active`、`cancelled`、`completed` |
| `schedule.query` | `start_date` | string | 否 | - | - | - | 查询开始日期，`YYYY-MM-DD` |
| `schedule.query` | `end_date` | string | 否 | - | - | - | 查询结束日期，`YYYY-MM-DD` |

### 出参

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.query` | `status` | string | 是 | `success` 或 `failure` |
| `schedule.query` | `message` | string | 是 | 结果描述 |
| `schedule.query` | `schedules` | array | 是 | 已物化日程列表，元素为 `schedule` |
| `schedule.query` | `future_occurrences` | array | 是 | 未来周期候选日程列表，元素为 `future_occurrence` |
| `schedule.query` | `exceptions` | array | 是 | 周期单次例外列表，元素为 `exception` |
| `schedule.query` | `result_count` | integer | 是 | `schedules` 与 `future_occurrences` 的总条数 |
| `schedule.query` | `recent` | object/null | 是 | 结构化结果中的最近一条日程 |
| `schedule.query` | `im_delivery` | string | 否 | `submitted`、`retryable_failed` 或 `failed` |

## Tool 3：`schedule.update`

### 入参

| 工具名称 | 参数 | 类型 | 必填 | 默认值 | 最小值 | 最大值 | 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `schedule.update` | `schedule_id` | integer | 条件必填 | - | `1` | - | 更新或取消已物化日程时使用的日程 ID，由 `schedule.query` 返回 |
| `schedule.update` | `rule_id` | integer | 条件必填 | - | `1` | - | 更新未来周期实例或整条周期规则时使用的规则 ID |
| `schedule.update` | `original_start_time` | string | 条件必填 | - | - | - | 未来周期实例的原始发生时间，`YYYY-MM-DD HH:mm:ss` |
| `schedule.update` | `event` | string | 否 | - | - | - | 新的日程标题 |
| `schedule.update` | `start_time` | string | 否 | - | - | - | 新的开始时间，`YYYY-MM-DD HH:mm:ss` |
| `schedule.update` | `end_time` | string | 否 | - | - | - | 新的结束时间，`YYYY-MM-DD HH:mm:ss` |
| `schedule.update` | `location` | string | 否 | - | - | - | 新的地点 |
| `schedule.update` | `notes` | string | 否 | - | - | - | 新的备注 |
| `schedule.update` | `status` | string | 否 | - | - | - | 更新日程状态：跳过某次周期日程时传 `cancelled`，恢复时传 `active` |
| `schedule.update` | `ignore_conflict` | boolean | 否 | `false` | - | - | 是否忽略时间冲突 |
| `schedule.update` | `repeat` | object | 否 | - | - | - | 更新周期规则时使用的新周期配置 |

`repeat` 字段定义见「通用数据结构 > `repeat`」。

约束：

- 已物化日程传 `schedule_id`。
- 未来周期单次传 `rule_id + original_start_time`。
- 更新整条周期规则传 `rule_id + repeat`。
- 上述目标至少传一组。

### 出参

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.update` | `status` | string | 是 | `success`、`conflict` 或 `failure` |
| `schedule.update` | `message` | string | 是 | 结果描述 |
| `schedule.update` | `schedule` | object \| null | 是 | 更新单次日程成功时返回 `schedule` |
| `schedule.update` | `rule` | object \| null | 是 | 更新周期规则成功时返回 `rule` |
| `schedule.update` | `exception` | object \| null | 是 | 修改或跳过未来周期单次时返回 `exception` |
| `schedule.update` | `conflicts` | array | 是 | 冲突日程列表；无冲突时为空数组 |

## Tool 4：`schedule.delete`

### 入参

| 工具名称 | 参数 | 类型 | 必填 | 默认值 | 最小值 | 最大值 | 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `schedule.delete` | `schedule_id` | integer | 条件必填 | - | `1` | - | 要删除或取消的单次日程 ID |
| `schedule.delete` | `rule_id` | integer | 条件必填 | - | `1` | - | 要删除或取消的周期规则 ID |
| `schedule.delete` | `original_start_time` | string | 条件必填 | - | - | - | 删除未来周期单次时使用的原始发生时间，`YYYY-MM-DD HH:mm:ss` |

约束：

- 已物化日程传 `schedule_id`。
- 整条周期规则传 `rule_id`。
- 未来周期单次传 `rule_id + original_start_time`。
- 上述三种目标至少传一组。

### 出参

| 工具名称 | 返回字段 | 类型 | 必返 | 说明 |
| --- | --- | --- | --- | --- |
| `schedule.delete` | `status` | string | 是 | `success` 或 `failure` |
| `schedule.delete` | `message` | string | 是 | 结果描述 |
| `schedule.delete` | `schedule` | object \| null | 是 | 传 `schedule_id` 删除成功时返回被取消的 `schedule` |
| `schedule.delete` | `rule` | object \| null | 是 | 传 `rule_id` 删除成功时返回被取消的 `rule` |
| `schedule.delete` | `exception` | object \| null | 是 | 删除未来周期单次成功时返回 `exception` |
