# 定时任务模块需求与设计文档 V1

本文档旨在定义 声活 VoiceLife 的定时任务模块。该模块为 App 的核心调度层模块，我参考了成熟解决方案 iCalendar 规范以及热门产品 Google Calendar、Microsoft Outlook 的 API 文档，分析其字段，理解其具体实现，结合自身业务场景，提出一套模块需求与设计文档。

## 1. 行业调研及成熟方案

1. Google Calendar API Document <https://developers.google.cn/workspace/calendar/api/concepts/events-calendars?hl=zh-cn>
2. iCalendar(RFC 5545) <https://icalendar.org/RFC-Specifications/iCalendar-RFC-5545/>
3. Microsoft Outlook <https://learn.microsoft.com/en-us/graph/api/resources/calendar?view=graph-rest-1.0&preserve-view=true>

根据调研结果，在对象处理方面，各家产品均做到了业务模型、调度模型、执行模型分离。结合 VoiceLife 当前“若干弱提醒 + 事件开始时强提醒”的业务约束，本模块进一步区分 occurrence 与 reminder trigger。与本模块的主要业务对象对应关系如下表：

| 本模块业务对象      | Google Calendar   | Outlook       | RFC5545       |
| --------- | ----------------- | ------------- |  ------------- |
| Schedule<br/>业务模型  | Event             | Event         | VEVENT        |
| TimerTask<br/>调度模型 | Master Event | Series Master | RRULE         |
| Instance<br/>执行模型  | Instance          | Occurrence    | RECURRENCE-ID |

## 2. 核心目标

将用户输入的日程数据转化为调度层数据，旨在规划每一个事件的 recurrence，使得系统按照用户意图将单一或周期性事件实例化，并在每次 occurrence 上派生若干弱提醒与一个可推迟的强提醒，让每一层执行对象均可以被精确改动与触发。

## 3. 核心概念定义

定义系统核心实体，以更精确地匹配业务模型。调度与提醒拆分关系如下：

- **`schedule_id`** **日程标识**
  - 对应上游模块的日程实体，是用户业务意图的引用。
  - 本模块仅读取与转发，不负责维护其业务状态。
  - 解决 What 的问题，即“用户想做什么”。
  
- **`task_id`** **定时任务标识**
  - 模块核心实体，由 `schedule_id` 派生而来。
  - 承载具体的调度策略与参数，如 `next_trigger_at`、`recurrence_rule` 等。
  - 负责把一条日程转换成可执行的调度任务，解决“系统应该如何安排触发”的问题。

- **`instance_id`** **实例标识**
  - 由定时任务生成的单次 occurrence 实体，代表某一个具体事件发生时刻。
  - 它是 reminder trigger 的父级对象，适合承载单次 occurrence 修改、例外处理和触发确认。
  - 解决“这一次具体 occurrence 是什么”的问题。

- **`reminder_trigger_id`** **提醒触发标识**
  - 由 `timer_instance` 和 `reminder_rule` 共同派生出的单次提醒动作实体。
  - 弱提醒 trigger 仅支持触发、送达、跳过或取消；强提醒 trigger 额外支持 snooze / dismiss。
  - 解决“这一次提醒动作本身发生了什么”的问题。


## 4. 核心业务流程

系统的运作主要分为五类流程：

### 4.1 上游日程接入与任务注册

1. 上游日程模块创建或更新一条 `schedule`，并将日程内容、开始时间、循环规则等传入本模块。
2. 本模块以 `schedule_id` 为引用，创建或更新对应的 `timer_task`。
3. `timer_task` 保存调度所需的核心字段，如 `next_trigger_at`、`recurrence_rule`、`effective_from` 等。
4. 任务注册后创建默认 `reminder_rule`：默认可包含一个事件开始前 10 分钟的弱提醒，以及一个事件开始时间的强提醒；用户通过 `UpsertReminderRules` 追加或覆盖提醒规则。
5. 对于一次性日程，任务通常只派生一个实例；对于周期日程，任务会持续维护下一次触发时间。

### 4.2 实例生成与提醒触发执行

1. 当 `timer_task` 到达 `next_trigger_at` 时，系统生成对应的 `timer_instance`。
2. 系统基于该 `timer_instance` 和生效中的 `reminder_rule` 派生出一个或多个 `reminder_trigger`。
3. 弱提醒 `reminder_trigger` 在对应偏移时间自动触发并结束，不支持 snooze。
4. 强提醒 `reminder_trigger` 在事件开始时间触发，支持 snooze / dismiss 等运行态操作。
5. 执行完成后，系统根据规则推进任务的下一次触发时间，并生成后续实例与提醒触发。
6. 如果某次提醒执行失败，系统只回写本次 `reminder_trigger` 状态，不直接破坏整条任务链路。

### 4.3 单次改动、例外处理与重算

1. 用户如果只修改“本次”，通常只影响某一个 `timer_instance`，不改整条 `timer_task`。
2. 用户如果修改“本次及以后”，系统以 `effective_from` 为边界，重算后续的调度规则、实例与提醒触发。
3. 用户如果取消某一条弱提醒规则，仅影响未来由该规则派生出的 `reminder_trigger`，不影响 occurrence 本身。
4. 用户如果取消日程，本模块会停止后续实例和提醒触发生成，并将对应任务标记为终止态。
5. 整个过程中，`schedule` 作为上游业务意图，`timer_task` 作为调度规则载体，`timer_instance` 作为 occurrence 结果，`reminder_trigger` 作为最终提醒动作，四者职责分离、单向影响。

### 4.4 用户查询与时间范围展开

1. 当用户查询“明天有什么安排”“下个月 5 号有哪些安排”时，系统应优先基于 `timer_task` 与 `recurrence_rule` 在目标时间范围内展开 occurrence。
2. 查询过程中需合并该范围内已存在的 `timer_instance`，用于叠加单次修改、跳过等 occurrence 例外状态。
3. 若某个未来 occurrence 尚未进入实例生成窗口，只要其规则可被展开，仍应在查询结果中返回。
4. 因此，用户查询结果不以实例是否已预生成作为前提；`timer_instance` 主要服务于 occurrence 承接与例外持久化。

### 4.5 提醒规则管理与运行态操作

1. 用户可新增、修改、取消弱提醒规则，例如“默认提前 10 分钟提醒”和“再提前 30 分钟提醒一次”。
2. 用户可关闭某条弱提醒规则，但弱提醒一旦触发后不支持 snooze。
3. 用户在强提醒触发中可执行 snooze / dismiss；系统作用对象为 `reminder_trigger`，而非 `timer_instance`。
4. 规则层变化只影响未来 `reminder_trigger` 的生成；已终态的历史提醒动作保留。

---

## 5. 模块接口

### 5.1 接口总览

| 接口 | 说明 |
| --- | --- |
| RegisterTimerTask | 注册定时任务 |
| UpdateTimerTask | 更新定时任务 |
| CancelTimerTask | 取消定时任务 |
| UpsertReminderRules | 创建或更新提醒规则 |
| DeleteReminderRule | 取消某条提醒规则 |
| ListCalendarView | 按时间范围查询用户可见安排 |
| ListReminderTriggers | 查询提醒触发列表 |
| SnoozeReminderTrigger | 推迟强提醒触发 |
| DismissReminderTrigger | 关闭强提醒触发 |

### 5.2 接口参数

#### 5.2.1 RegisterTimerTask

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| schedule_id | string | 是 | 来源于上游 schedule | 日程 ID |
| start_at | datetime | 是 | ISO 8601 | 首次触发时间 |
| recurrence_rule | object | 否 | 一次性日程可为空 | 周期规则 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 定时任务 ID |
| status | string | 枚举 | 注册结果状态，通常为 `active` |
| next_trigger_at | datetime | 可空 | 下一次预计触发时间 |

#### 5.2.2 UpdateTimerTask

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| task_id | string | 是 | 非空 | 定时任务 ID |
| schedule_id | string | 否 | 来源于上游 schedule，需与 task_id 绑定记录一致 | 关联日程 ID |
| start_at | datetime | 否 | ISO 8601 | 更新后的开始时间 |
| recurrence_rule | object | 否 | `change_scope=single` 时不适用 | 更新后的周期规则 |
| change_scope | string | 是 | `single` / `future` / `all` | 修改范围 |
| instance_id | string | 否 | `change_scope=single` 时可用 | 目标实例 ID |
| target_occurrence_at | datetime | 否 | `change_scope=single` 时可用 | 原计划触发时间 |
| effective_from | datetime | 否 | `change_scope=future`  时可用 | 生效开始时间 |


**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 被更新的任务 ID |
| status | string | 枚举 | 更新后的任务状态；`single` 场景下目标实例通常为 `modified` |
| next_trigger_at | datetime | 可空 | 重算后的下一次触发时间 |
| instance_id | string | 可空 | `single` 场景下目标实例 ID |
| override_fields | object | 可空 | `single` 场景下本次覆盖字段 |
| affected_instance_count | integer | 可空，>= 0 | `future` / `all` 场景下受影响的实例数量 |

#### 5.2.3 CancelTimerTask

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| task_id | string | 是 | 非空 | 定时任务 ID |
| schedule_id | string | 否 | 来源于上游 schedule，需与 task_id 绑定记录一致 | 关联日程 ID |
| change_scope | string | 是 | `single` / `future` / `all` | 取消范围 |
| instance_id | string | 否 | `change_scope=single` 时可用 | 目标实例 ID |
| target_occurrence_at | datetime | 否 | `change_scope=single` 时可用 | 原计划触发时间 |
| effective_from | datetime | 否 | `change_scope=future` 时可用 | 向后取消的起点 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 被取消的任务 ID |
| instance_id | string | 可空 | `single` 场景下目标实例 ID |
| status | string | 枚举 | 整体取消通常为 `terminated`；`single` 场景下目标实例通常为 `skipped` |
| affected_instance_count | integer | 可空，>= 0 | `future` / `all` 场景下受影响的实例数量 |

#### 5.2.4 UpsertReminderRules

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| task_id | string | 是 | 非空 | 定时任务 ID |
| schedule_id | string | 否 | 可为空 | 关联日程 ID |
| rules | array<object> | 是 | 至少 1 条 | 要创建或更新的提醒规则 |

`rules` 子项建议字段：

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_rule_id | string | 可空 | 更新已有规则时传入 |
| reminder_type | string | 枚举，必填 | `weak` / `strong` |
| offset_minutes | integer | 必填 | 相对 occurrence 开始时间的偏移分钟；弱提醒通常小于 0，强提醒通常为 0 |
| max_snooze_count | integer | 可空，>= 0 | 强提醒允许的最大推迟次数 |
| snooze_interval_minutes | integer | 可空，> 0 | 强提醒默认推迟间隔 |
| channel | string | 可空 | 提醒渠道，如 `voice` / `im` |
| source | string | 枚举，可空 | `system_default` / `user_defined` |

约束：
- 同一 `task_id` 下只允许 1 条 `reminder_type=strong && offset_minutes=0` 的强提醒规则。
- `weak` 规则不支持 snooze。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 所属任务 ID |
| reminder_rules | array<object> | 可空 | 创建或更新后的完整规则列表 |

#### 5.2.5 DeleteReminderRule

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| reminder_rule_id | string | 是 | 非空 | 提醒规则 ID |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_rule_id | string | 唯一 | 被取消的提醒规则 ID |
| status | string | 枚举 | 通常为 `disabled` |
| affected_trigger_count | integer | 可空，>= 0 | 受影响的未来提醒触发数量 |

#### 5.2.6 ListCalendarView

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| range_start | datetime | 是 | ISO 8601 | 查询开始时间 |
| range_end | datetime | 是 | ISO 8601，且必须大于 `range_start` | 查询结束时间 |
| schedule_id | string | 否 | 可为空 | 指定日程 ID；为空时表示查询用户可访问范围内的全部日程 |
| status | string | 否 | 枚举 | 查询结果状态过滤，通常为用户视图状态 |
| page | integer | 否 | 大于 0 | 页码，从 1 开始 |
| page_size | integer | 否 | 1 到 100 | 每页数量 |
| sort_by | string | 否 | 枚举 | 排序字段，默认 `planned_start_at` |
| sort_order | string | 否 | 枚举 | 排序方向，`asc` / `desc` |

约束：
- `ListCalendarView` 面向用户查询语义，服务端应基于 `timer_task` 与 `recurrence_rule` 展开目标时间范围内的 occurrence。
- 服务端应合并该时间范围内已存在的 `timer_instance`，用于叠加 `modified` / `completed` / `skipped` 等 occurrence 例外状态。
- 即使目标 occurrence 尚未进入实例生成窗口，只要符合规则且未被取消，仍应返回到结果中。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| occurrences | array<object> | 可空 | 用户在该时间范围内可见的安排列表 |
| total | integer | 大于等于 0 | 安排总数 |
| page | integer | 大于 0 | 当前页码 |
| page_size | integer | 大于 0 | 每页数量 |
| has_more | boolean | 非空 | 是否还有下一页 |

`occurrences` 建议最小字段：

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| occurrence_id | string | 非空 | 本次 occurrence 的稳定标识，可由 `task_id + planned_start_at` 派生 |
| schedule_id | string | 非空 | 所属日程 ID |
| task_id | string | 非空 | 所属定时任务 ID |
| instance_id | string | 可空 | 若该 occurrence 已物化为实例，则返回实例 ID |
| title | string | 可空 | 用户可见标题，由上游业务提供 |
| planned_start_at | datetime | 非空 | 按规则展开得到的原始开始时间 |
| planned_end_at | datetime | 可空 | 按规则展开得到的原始结束时间 |
| actual_trigger_at | datetime | 可空 | 若被推迟或改单次触发时间，则返回实际触发时间 |
| status | string | 枚举 | 用户视图下的当前有效状态 |
| is_recurring | boolean | 非空 | 是否来自周期规则 |
| is_exception | boolean | 非空 | 是否叠加了单次例外 |
| override_fields | object | 可空 | 单次覆盖字段 |

#### 5.2.7 ListReminderTriggers

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| task_id | string | 否 | 可为空 | 定时任务 ID |
| instance_id | string | 否 | 可为空 | occurrence 实例 ID |
| schedule_id | string | 否 | 可为空 | 日程 ID |
| reminder_type | string | 否 | `weak` / `strong` | 提醒类型 |
| status | string | 否 | reminder trigger 状态枚举 | 提醒触发状态 |
| range_start | datetime | 否 | ISO 8601 | 实际触发时间范围起点（包含） |
| range_end | datetime | 否 | ISO 8601，且必须大于 `range_start` | 实际触发时间范围终点（不包含） |
| page | integer | 否 | 大于 0，默认 1 | 页码 |
| page_size | integer | 否 | 1 到 100，默认 20 | 每页数量 |
| sort_by | string | 否 | `actual_trigger_at` / `planned_trigger_at` / `created_at` | 排序字段，默认 `actual_trigger_at` |
| sort_order | string | 否 | `asc` / `desc` | 排序方向，默认 `asc` |

约束：
- 至少提供 `task_id`、`instance_id`、`schedule_id` 或 `range_start + range_end` 中的一组查询条件。
- `range_start` 与 `range_end` 必须同时提供，按左闭右开区间 `[range_start, range_end)` 过滤 `actual_trigger_at`。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_triggers | array<object> | 可空 | 符合条件的提醒触发列表 |
| total | integer | 大于等于 0 | 符合条件的提醒触发总数 |
| page | integer | 大于 0 | 当前页码 |
| page_size | integer | 1 到 100 | 每页数量 |
| has_more | boolean | 非空 | 是否还有下一页 |

#### 5.2.8 SnoozeReminderTrigger

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| reminder_trigger_id | string | 是 | 非空 | 提醒触发 ID |
| delay_minutes | integer | 是 | 大于 0 | 推迟时长，单位分钟 |

约束：
- 仅 `reminder_type=strong` 的触发允许调用。
- 弱提醒调用该接口时应返回参数或状态错误。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_trigger_id | string | 唯一 | 被推迟的提醒触发 ID |
| status | string | 枚举 | 通常为 `snoozed` |
| actual_trigger_at | datetime | 非空 | 推迟后的实际触发时间 |
| snooze_count | integer | 大于等于 0 | 推迟后的累计次数 |

#### 5.2.9 DismissReminderTrigger

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| reminder_trigger_id | string | 是 | 非空 | 提醒触发 ID |

约束：
- 通常仅强提醒触发支持 dismiss；弱提醒如果已送达则无需额外关闭。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_trigger_id | string | 唯一 | 被关闭的提醒触发 ID |
| status | string | 枚举 | 通常为 `dismissed` |


### 5.3 下游契约
适用对象：
- `IM` 模块
- `语音(输出)`模块

建议最小契约字段：

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| event_type | string | 是 | 事件类型，如 `instance_created` / `reminder_triggered` / `reminder_snoozed` / `reminder_dismissed` / `task_cancelled` / `task_updated` |
| event_id | string | 是 | 事件唯一标识，用于幂等 |
| task_id | string | 是 | 定时任务 ID |
| instance_id | string | 否 | 实例 ID；非实例级事件可为空 |
| reminder_rule_id | string | 否 | 提醒规则 ID；非提醒级事件可为空 |
| reminder_trigger_id | string | 否 | 提醒触发 ID；非提醒级事件可为空 |
| schedule_id | string | 是 | 关联日程 ID |
| planned_at | datetime | 否 | 原始计划触发时间 |
| trigger_at | datetime | 是 | 实际触发时间或生效时间 |
| status | string | 是 | 事件对应状态 |
| payload | object | 否 | 下游展示或播报所需内容 |
| occurred_at | datetime | 是 | 事件产生时间 |

约束：
- 同一 `event_id` 在下游必须幂等处理。
- 定时任务模块不定义 IM 的落库、展示和重试实现。


### 5.4 状态约定

- `timer_task.status`
  - `active`：运行中。
  - `terminated`：终止态，不再生成新实例。
- `timer_instance.status`
  - 非终态：`pending`、`modified`、`triggered`。
  - 终态：`completed`、`skipped`。
  - 允许流转：
    - `pending -> modified / triggered / skipped`
    - `modified -> triggered / skipped`
    - `triggered -> completed / skipped`
  - `completed`、`skipped` 为终态，不再回退。
- `reminder_rule.status`
  - `active`：启用中。
  - `disabled`：关闭中，不再为未来实例派生触发。
- `reminder_trigger.status`
  - 弱提醒非终态：`pending`、`triggered`。
  - 弱提醒终态：`delivered`、`skipped`、`cancelled`、`failed`。
  - 强提醒非终态：`pending`、`triggered`、`snoozed`。
  - 强提醒终态：`delivered`、`dismissed`、`cancelled`、`failed`。
  - 允许流转：
    - `pending -> triggered / skipped / cancelled`
    - `triggered -> delivered / snoozed / dismissed / failed`
    - `snoozed -> triggered / dismissed / failed`



## 6. 主要数据模型

### 6.1 `timer_task`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 定时任务唯一标识 |
| schedule_id | string | 外键，非空 | 关联的日程 ID |
| status | string | 枚举，非空 | 任务状态，`active` / `terminated` |
| next_trigger_at | datetime | 可空 | 下一次预计触发时间 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |

### 6.2 `recurrence_rule`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| frequency | string | 枚举，非空 | 周期频率，仅支持 `day` / `week` / `month` / `year`，分别表示每日、每周、每月、每年 |
| start_at | datetime | 非空 | 周期锚点时间 |
| timezone | string | 非空 | 时区，当前一期统一使用 `+08:00` |
| by_weekdays | array<string> | 可空 | 每周循环时指定星期几 |
| by_month_day | array<integer> | 可空 | 每月或每年循环时指定日期 |
| by_month | array<integer> | 可空 | 每年循环时指定月份 |

约束：
- 循环固定为每 1 日、每 1 周、每 1 月或每 1 年，不支持自定义周期间隔。
- 循环任务持续生效，直至通过 `CancelTimerTask` 终止，不支持按截止时间或执行次数自动结束。
- `frequency=day` 时，不使用 `by_weekdays`、`by_month_day` 和 `by_month`。
- `frequency=week` 时，可使用 `by_weekdays`；未传时使用 `start_at` 对应的星期。
- `frequency=month` 时，可使用 `by_month_day`；未传时使用 `start_at` 对应的日期。
- `frequency=year` 时，可使用 `by_month` 和 `by_month_day`；未传时使用 `start_at` 对应的月份和日期。

### 6.3 `timer_instance`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 实例唯一标识 |
| task_id | string | 外键，非空 | 所属定时任务 ID |
| planned_at | datetime | 非空 | occurrence 的原始开始时间 |
| planned_end_at | datetime | 可空 | occurrence 的原始结束时间 |
| status | string | 枚举，非空 | 实例状态，支持 `pending` / `modified` / `triggered` / `completed` / `skipped` |
| override_fields | object | 可空 | 本次实例相对原规则的覆盖字段 |
| last_action_at | datetime | 可空 | 最后一次用户操作或系统状态变更时间 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |

补充说明：
- `timer_instance` 是持久化对象，可由近端调度窗口预生成，也可在单次修改、单次取消等需要承载例外时按需生成。
- 同一 `task_id` 下，`planned_at` 应具备唯一性约束，用于保障 instance 幂等与例外合并。

### 6.4 `reminder_rule`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 提醒规则唯一标识 |
| task_id | string | 外键，非空 | 所属定时任务 ID |
| reminder_type | string | 枚举，非空 | `weak` / `strong` |
| offset_minutes | integer | 非空 | 相对 occurrence 开始时间的偏移分钟 |
| max_snooze_count | integer | 可空，>= 0 | 强提醒允许的最大推迟次数 |
| snooze_interval_minutes | integer | 可空，> 0 | 强提醒默认推迟间隔 |
| channel | string | 可空 | 提醒渠道 |
| source | string | 枚举，非空 | `system_default` / `user_defined` |
| status | string | 枚举，非空 | `active` / `disabled` |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |

约束：
- 同一 `task_id` 下，只允许一条 `reminder_type=strong && offset_minutes=0` 的强提醒规则处于 `active`。
- 弱提醒规则允许多条并存且不支持 snooze。

### 6.5 `reminder_trigger`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 提醒触发唯一标识 |
| reminder_rule_id | string | 外键，非空 | 来源提醒规则 ID |
| task_id | string | 外键，非空 | 所属定时任务 ID |
| instance_id | string | 外键，非空 | 所属 occurrence 实例 ID |
| reminder_type | string | 枚举，非空 | `weak` / `strong` |
| planned_trigger_at | datetime | 非空 | 按规则计算出的原始提醒时间 |
| actual_trigger_at | datetime | 非空 | 当前实际触发时间；snooze 后会变化 |
| status | string | 枚举，非空 | 提醒触发状态 |
| snooze_count | integer | 非空，>= 0 | 当前提醒已被推迟次数 |
| delivered_at | datetime | 可空 | 成功送达时间 |
| last_action_at | datetime | 可空 | 最后一次用户操作或系统状态变更时间 |
| payload | object | 可空 | 下游播报或展示所需内容快照 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |

约束：
- 同一 `instance_id + reminder_rule_id` 下仅允许存在一条未删除 `reminder_trigger`。
- 弱提醒 `reminder_trigger` 不允许进入 `snoozed` 状态。
