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

- **`reminder_rule_id`** **提醒规则标识**
  - 由定时任务维护的提醒策略实体，代表“围绕一次 occurrence，系统需要提前多久或到点如何提醒”。
  - 允许配置若干弱提醒规则，以及一个事件开始时间的强提醒规则。
  - 解决“系统应该派生哪些提醒动作”的问题。

- **`reminder_trigger_id`** **提醒触发标识**
  - 由 `timer_instance` 和 `reminder_rule` 共同派生出的单次提醒动作实体。
  - 弱提醒 trigger 仅支持触发、送达、跳过或取消；强提醒 trigger 额外支持 snooze / dismiss。
  - 解决“这一次提醒动作本身发生了什么”的问题。


## 3. 核心业务流程

系统的运作主要分为五类流程：

### 流程一：上游日程接入与任务注册

1. 上游日程模块创建或更新一条 `schedule`，并将日程内容、开始时间、循环规则等传入本模块。
2. 本模块以 `schedule_id` 为引用，创建或更新对应的 `timer_task`。
3. `timer_task` 保存调度所需的核心字段，如 `next_trigger_at`、`recurrence_rule`、`effective_from` 等。
4. 本模块根据 `reminder_config` 编译生成若干 `reminder_rule`：默认可包含一个事件开始前 10 分钟的弱提醒，以及一个事件开始时间的强提醒；用户也可追加或覆盖弱提醒规则。
5. 对于一次性日程，任务通常只派生一个实例；对于周期日程，任务会持续维护下一次触发时间。

### 流程二：实例生成与提醒触发执行

1. 当 `timer_task` 到达 `next_trigger_at` 时，系统生成对应的 `timer_instance`。
2. 系统基于该 `timer_instance` 和生效中的 `reminder_rule` 派生出一个或多个 `reminder_trigger`。
3. 弱提醒 `reminder_trigger` 在对应偏移时间自动触发并结束，不支持 snooze。
4. 强提醒 `reminder_trigger` 在事件开始时间触发，支持 snooze / dismiss 等运行态操作。
5. 执行完成后，系统根据规则推进任务的下一次触发时间，并生成后续实例与提醒触发。
6. 如果某次提醒执行失败，系统只回写本次 `reminder_trigger` 状态，不直接破坏整条任务链路。

### 流程三：单次改动、例外处理与重算

1. 用户如果只修改“本次”，通常只影响某一个 `timer_instance`，不改整条 `timer_task`。
2. 用户如果修改“本次及以后”，系统以 `effective_from` 为边界，重算后续的调度规则、实例与提醒触发。
3. 用户如果取消某一条弱提醒规则，仅影响未来由该规则派生出的 `reminder_trigger`，不影响 occurrence 本身。
4. 用户如果取消日程，本模块会停止后续实例和提醒触发生成，并将对应任务标记为终止态。
5. 整个过程中，`schedule` 作为上游业务意图，`timer_task` 作为调度规则载体，`timer_instance` 作为 occurrence 结果，`reminder_trigger` 作为最终提醒动作，四者职责分离、单向影响。

### 流程四：用户查询与时间范围展开

1. 当用户查询“明天有什么安排”“下个月 5 号有哪些安排”时，系统应优先基于 `timer_task` 与 `recurrence_rule` 在目标时间范围内展开 occurrence。
2. 查询过程中需合并该范围内已存在的 `timer_instance`，用于叠加单次修改、跳过等 occurrence 例外状态。
3. 若某个未来 occurrence 尚未进入实例生成窗口，只要其规则可被展开，仍应在查询结果中返回。
4. 因此，用户查询结果不以实例是否已预生成作为前提；`timer_instance` 主要服务于 occurrence 承接与例外持久化。

### 流程五：提醒规则管理与运行态操作

1. 用户可新增、修改、取消弱提醒规则，例如“默认提前 10 分钟提醒”和“再提前 30 分钟提醒一次”。
2. 用户可关闭某条弱提醒规则，但弱提醒一旦触发后不支持 snooze。
3. 用户在强提醒触发中可执行 snooze / dismiss；系统作用对象为 `reminder_trigger`，而非 `timer_instance`。
4. 规则层变化只影响未来 `reminder_trigger` 的生成；已终态的历史提醒动作保留。


---

# 定时任务模块接口设计文档

## 2. 模块接口

基本关系：`Schedule` 日程(上游, What) -> `TimerTask` 任务系列 -> `TimerInstance` occurrence -> `ReminderTrigger` 具体提醒动作
- `schedule`：日程模块中的业务记录，表示“用户想在什么时间做什么事”。
- `timer_task`：定时任务模块中的调度记录，表示“系统根据这条日程，应该如何安排后续触发”。
- `timer_instance`：某一次具体 occurrence 实例，表示“这一次事件本身”。
- `reminder_rule`：作用在 `timer_task` 上的提醒规则，表示“围绕每次 occurrence 需要派生哪些提醒”。
- `reminder_trigger`：某一次具体提醒动作，表示“这一次提醒本身”。
- 上游 `schedule` 的状态遵循 `active` / `completed` / `cancelled`，本模块只读取不维护。

约束:
- 一条 `schedule` 可以对应一条 `timer_task`。
- 一条 `timer_task` 可以派生出一条或多条 `timer_instance`。
- 一条 `timer_task` 可以维护一条或多条 `reminder_rule`。
- 一条 `timer_instance` 可以派生一条或多条 `reminder_trigger`。
- 一次性日程通常对应 1 个实例。
- 周期日程通常会不断生成后续实例，但一般只维护最近一次或一个较小时间窗口内的实例。
- `timer_instance` 主要承接 occurrence 执行态和例外态；未来时间范围查询不应仅依赖实例表。
- 默认提醒策略可包含一个 `offset_minutes = -10` 的弱提醒规则和一个 `offset_minutes = 0` 的强提醒规则。
- 弱提醒允许创建、修改、取消，但不允许 snooze。
- 强提醒通常位于事件开始时间，允许 snooze / dismiss。
- 用户查询某个未来时间范围时，应基于 `timer_task` + `recurrence_rule` 展开 occurrence，并叠加实例级例外。
- 当用户修改“单次”时，通常是对某个 `timer_instance` 做例外处理，不直接改变整条 `timer_task` 的周期规则。
- `single` 场景下允许仅通过 `target_occurrence_at` 定位某个 future occurrence；若对应实例尚不存在，服务端可按需创建一个 `timer_instance` 用于承载本次例外。
- 当用户修改“本次及以后”时，需要以 `effective_from` 为边界，重算该时间点之后的任务和实例。
- `change_scope` 语义如下：
  - `single`：仅作用于某一个 `timer_instance`。
  - `future`：从 `effective_from` 开始，作用于后续未终态实例及后续调度规则。
  - `all`：作用于整个 `timer_task` 系列；历史已终态实例保留，未终态实例与后续规则按新配置重算。
- `UpdateTimerTask` 的 `all` 场景会保留历史终态实例，仅重算当前未终态实例与后续规则。
- `CancelTimerTask` 的 `all` 场景会将 `timer_task` 置为 `terminated`，并将所有未终态实例标记为 `skipped`，历史终态实例保留。
- `GenerateInstances` 需保证实例级幂等，同一 `task_id` 下相同 `planned_at` 不应重复生成；同一 `instance_id + reminder_rule_id` 下的 `reminder_trigger` 也不应重复生成。
- `GenerateInstances` 的目标是为 reminder trigger 执行、近端调度和例外操作物化 instance 与 trigger，不作为未来日程查询正确性的唯一保障。
- `RegisterTimerTask` 对同一 `schedule_id` 采用幂等 upsert，已存在时更新原 `timer_task`，不重复创建。
- `taskId` 为主标识，`schedule_id` 为辅助来源字段；若传入则必须与 `taskId` 绑定记录一致，未传则按 `taskId` 处理。
- 所有时间字段统一使用 ISO 8601 表示，实例生成与重算优先采用 `recurrence_rule.timezone`，未配置时默认 `+08:00`。

上述各层实体为上下游关系，影响自上而下，且不可反向影响。

### 2.1 接口总览

| 接口 | Method | Path | 说明 |
| --- | --- | --- | --- |
| RegisterTimerTask | POST | `/v1/timer-tasks` | 注册定时任务 |
| UpdateTimerTask | PATCH | `/v1/timer-tasks/{taskId}` | 更新定时任务 |
| CancelTimerTask | DELETE | `/v1/timer-tasks/{taskId}` | 取消定时任务 |
| GenerateInstances | POST | `/v1/timer-tasks/{taskId}/instances` | 基于任务生成实例 |
| UpsertReminderRules | PUT | `/v1/timer-tasks/{taskId}/reminder-rules` | 创建或更新提醒规则 |
| DeleteReminderRule | DELETE | `/v1/reminder-rules/{reminderRuleId}` | 取消某条提醒规则 |
| ListCalendarView | GET | `/v1/calendar-view` | 按时间范围查询用户可见安排 |
| ListInstances | GET | `/v1/timer-instances` | 查询实例列表 |
| ListReminderTriggers | GET | `/v1/reminder-triggers` | 查询提醒触发列表 |
| SnoozeReminderTrigger | POST | `/v1/reminder-triggers/{reminderTriggerId}/snooze` | 推迟强提醒触发 |
| DismissReminderTrigger | POST | `/v1/reminder-triggers/{reminderTriggerId}/dismiss` | 关闭强提醒触发 |

### 2.2 接口参数

#### 2.2.1 RegisterTimerTask

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| schedule_id | string | body | 是 | 来源于上游 schedule | 日程 ID |
| start_at | datetime | body | 是 | ISO 8601 | 首次触发时间 |
| recurrence_rule | object | body | 否 | 一次性日程可为空 | 周期规则 |
| reminder_config | object | body | 否 | 写入侧提醒配置 | 服务端据此编译默认 `reminder_rule` 列表 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 定时任务 ID |
| status | string | 枚举 | 注册结果状态，通常为 `active` |
| next_trigger_at | datetime | 可空 | 下一次预计触发时间 |

#### 2.2.2 UpdateTimerTask

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| taskId | string | path | 是 | 路径参数 | 定时任务 ID |
| schedule_id | string | body | 否 | 来源于上游 schedule，需与 taskId 绑定记录一致 | 关联日程 ID |
| start_at | datetime | body | 否 | ISO 8601 | 更新后的开始时间 |
| recurrence_rule | object | body | 否 | `change_scope=single` 时不适用 | 更新后的周期规则 |
| reminder_config | object | body | 否 | 写入侧提醒配置 | 服务端据此重算默认 `reminder_rule` 列表 |
| change_scope | string | body | 是 | `single` / `future` / `all` | 修改范围 |
| instance_id | string | body | 否 | `change_scope=single` 时可用 | 目标实例 ID |
| target_occurrence_at | datetime | body | 否 | `change_scope=single` 时可用 | 原计划触发时间 |
| effective_from | datetime | body | 否 | `change_scope=future`  时可用 | 生效开始时间 |


**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 被更新的任务 ID |
| status | string | 枚举 | 更新后的任务状态；`single` 场景下目标实例通常为 `modified` |
| next_trigger_at | datetime | 可空 | 重算后的下一次触发时间 |
| instance_id | string | 可空 | `single` 场景下目标实例 ID |
| override_fields | object | 可空 | `single` 场景下本次覆盖字段 |
| affected_instance_count | integer | 可空，>= 0 | `future` / `all` 场景下受影响的实例数量 |

#### 2.2.3 CancelTimerTask

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| taskId | string | path | 是 | 路径参数 | 定时任务 ID |
| schedule_id | string | body | 否 | 来源于上游 schedule，需与 taskId 绑定记录一致 | 关联日程 ID |
| change_scope | string | body | 是 | `single` / `future` / `all` | 取消范围 |
| instance_id | string | body | 否 | `change_scope=single` 时可用 | 目标实例 ID |
| target_occurrence_at | datetime | body | 否 | `change_scope=single` 时可用 | 原计划触发时间 |
| effective_from | datetime | body | 否 | `change_scope=future` 时可用 | 向后取消的起点 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 被取消的任务 ID |
| instance_id | string | 可空 | `single` 场景下目标实例 ID |
| status | string | 枚举 | 整体取消通常为 `terminated`；`single` 场景下目标实例通常为 `skipped` |
| affected_instance_count | integer | 可空，>= 0 | `future` / `all` 场景下受影响的实例数量 |

#### 2.2.4 GenerateInstances

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| taskId | string | path | 是 | 路径参数 | 定时任务 ID |
| window_start | datetime | body | 是 | ISO 8601 | 生成窗口开始时间 |
| window_end | datetime | body | 是 | ISO 8601 | 生成窗口结束时间 |
| limit | integer | body | 否 | 大于 0 | 最多生成数量 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 所属任务 ID |
| instances | array<object> | 可空 | 生成出的实例列表 |
| reminder_triggers | array<object> | 可空 | 由实例和提醒规则派生出的提醒触发列表 |

约束：
- `GenerateInstances` 在生成 `timer_instance` 后，应同步为窗口内命中的 `reminder_rule` 派生 `reminder_trigger`。

#### 2.2.5 UpsertReminderRules

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| taskId | string | path | 是 | 路径参数 | 定时任务 ID |
| schedule_id | string | body | 否 | 可为空 | 关联日程 ID |
| rules | array<object> | body | 是 | 至少 1 条 | 要创建或更新的提醒规则 |

`rules` 子项建议字段：

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_rule_id | string | 可空 | 更新已有规则时传入 |
| reminder_type | string | 枚举，必填 | `weak` / `strong` |
| offset_minutes | integer | 必填 | 相对 occurrence 开始时间的偏移分钟；弱提醒通常小于 0，强提醒通常为 0 |
| enabled | boolean | 非空 | 是否启用 |
| can_snooze | boolean | 非空 | 仅 `strong` 可为 `true` |
| max_snooze_count | integer | 可空，>= 0 | 强提醒允许的最大推迟次数 |
| snooze_interval_minutes | integer | 可空，> 0 | 强提醒默认推迟间隔 |
| channel | string | 可空 | 提醒渠道，如 `voice` / `im` |
| source | string | 枚举，可空 | `system_default` / `user_defined` |

约束：
- 同一 `task_id` 下只允许 1 条 `reminder_type=strong && offset_minutes=0` 的强提醒规则。
- `weak` 规则必须满足 `can_snooze=false`。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| task_id | string | 唯一 | 所属任务 ID |
| reminder_rules | array<object> | 可空 | 创建或更新后的完整规则列表 |

#### 2.2.6 DeleteReminderRule

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| reminderRuleId | string | path | 是 | 路径参数 | 提醒规则 ID |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_rule_id | string | 唯一 | 被取消的提醒规则 ID |
| status | string | 枚举 | 通常为 `disabled` |
| affected_trigger_count | integer | 可空，>= 0 | 受影响的未来提醒触发数量 |

#### 2.2.7 ListCalendarView

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| range_start | datetime | query | 是 | ISO 8601 | 查询开始时间 |
| range_end | datetime | query | 是 | ISO 8601，且必须大于 `range_start` | 查询结束时间 |
| schedule_id | string | query | 否 | 可为空 | 指定日程 ID；为空时表示查询用户可访问范围内的全部日程 |
| status | string | query | 否 | 枚举 | 查询结果状态过滤，通常为用户视图状态 |
| page | integer | query | 否 | 大于 0 | 页码，从 1 开始 |
| page_size | integer | query | 否 | 1 到 100 | 每页数量 |
| sort_by | string | query | 否 | 枚举 | 排序字段，默认 `planned_start_at` |
| sort_order | string | query | 否 | 枚举 | 排序方向，`asc` / `desc` |

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

#### 2.2.8 ListInstances

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| task_id | string | query | 否 | 可为空 | 定时任务 ID |
| schedule_id | string | query | 否 | 可为空 | 日程 ID |
| range_start | datetime | query | 否 | ISO 8601 | 查询开始时间 |
| range_end | datetime | query | 否 | ISO 8601 | 查询结束时间 |
| status | string | query | 否 | 枚举 | 实例状态过滤条件 |
| page | integer | query | 否 | 大于 0 | 页码，从 1 开始 |
| page_size | integer | query | 否 | 1 到 100 | 每页数量 |
| sort_by | string | query | 否 | 枚举 | 排序字段，默认 `planned_at` |
| sort_order | string | query | 否 | 枚举 | 排序方向，`asc` / `desc` |

约束：
- 至少提供 `task_id`、`schedule_id` 或 `range_start` + `range_end` 中的一组条件，否则服务端返回参数错误。
- `page` 和 `page_size` 仅在查询结果分页时生效。
- `ListInstances` 仅返回已物化的 `timer_instance`；若某个未来 occurrence 尚未生成实例，则不会出现在结果中。
- `ListInstances` 适用于 occurrence 执行态、实例操作态和审计场景，不承担未来时间范围内完整日程查询职责。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| instances | array<object> | 可空 | 符合条件的 occurrence 实例列表 |
| total | integer | 大于等于 0 | 实例总数 |
| page | integer | 大于 0 | 当前页码 |
| page_size | integer | 大于 0 | 每页数量 |
| has_more | boolean | 非空 | 是否还有下一页 |

#### 2.2.9 ListReminderTriggers

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| task_id | string | query | 否 | 可为空 | 定时任务 ID |
| schedule_id | string | query | 否 | 可为空 | 日程 ID |
| instance_id | string | query | 否 | 可为空 | occurrence 实例 ID |
| reminder_rule_id | string | query | 否 | 可为空 | 提醒规则 ID |
| range_start | datetime | query | 否 | ISO 8601 | 查询开始时间 |
| range_end | datetime | query | 否 | ISO 8601 | 查询结束时间 |
| reminder_type | string | query | 否 | 枚举 | `weak` / `strong` |
| status | string | query | 否 | 枚举 | 提醒触发状态过滤条件 |
| page | integer | query | 否 | 大于 0 | 页码，从 1 开始 |
| page_size | integer | query | 否 | 1 到 100 | 每页数量 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_triggers | array<object> | 可空 | 符合条件的提醒触发列表 |
| total | integer | 大于等于 0 | 提醒触发总数 |
| page | integer | 大于 0 | 当前页码 |
| page_size | integer | 大于 0 | 每页数量 |
| has_more | boolean | 非空 | 是否还有下一页 |

#### 2.2.10 SnoozeReminderTrigger

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| reminderTriggerId | string | path | 是 | 路径参数 | 提醒触发 ID |
| delay_minutes | integer | body | 是 | 大于 0 | 推迟时长，单位分钟 |

约束：
- 仅 `reminder_type=strong` 且 `can_snooze=true` 的触发允许调用。
- 弱提醒调用该接口时应返回参数或状态错误。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_trigger_id | string | 唯一 | 被推迟的提醒触发 ID |
| status | string | 枚举 | 通常为 `snoozed` |
| actual_trigger_at | datetime | 可空 | 推迟后的实际触发时间 |
| snooze_count | integer | 大于等于 0 | 推迟后的累计次数 |

#### 2.2.11 DismissReminderTrigger

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| reminderTriggerId | string | path | 是 | 路径参数 | 提醒触发 ID |

约束：
- 通常仅强提醒触发支持 dismiss；弱提醒如果已送达则无需额外关闭。

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| reminder_trigger_id | string | 唯一 | 被关闭的提醒触发 ID |
| status | string | 枚举 | 通常为 `dismissed` |


### 2.3 下游契约



适用对象：
- `IM` 模块
- `语音(输出)`模块

建议最小契约字段：

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| event_type | string | 是 | 事件类型，如 `instance_created` / `reminder_triggered` / `reminder_snoozed` / `reminder_dismissed` / `task_paused` / `task_resumed` / `task_cancelled` / `task_updated` |
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


### 2.4 状态约定

- `timer_task.status`
  - `active`：运行中。
  - `paused`：暂停中，恢复前不生成新实例。
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
- `calendar_view_occurrence.status`
  - 查询态候选值可复用 instance 语义，如 `pending`、`modified`、`triggered`、`completed`、`skipped`。
  - 若某个 occurrence 尚未物化实例，但按规则在查询范围内有效，可返回 `pending` 作为默认可见状态。


## 3. 数据模型

### 3.1 `timer_task`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 定时任务唯一标识 |
| schedule_id | string | 外键，非空 | 关联的日程 ID |
| status | string | 枚举，非空 | 任务状态，`active` / `paused` / `terminated` |
| next_trigger_at | datetime | 可空 | 下一次预计触发时间 |
| default_reminder_config | object | 可空 | 写入侧提醒配置快照，用于编译默认提醒规则 |
| paused_until | datetime | 可空 | 暂停恢复时间，仅当 `status=paused` 时有效 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |

### 3.2 `recurrence_rule`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| frequency | string | 枚举，非空 | 周期频率，支持 `day` / `week` / `month` / `year` |
| interval | integer | 非空，>= 1 | 周期间隔，例如每 2 天、每 3 周 |
| start_at | datetime | 非空 | 周期锚点时间 |
| timezone | string | 非空 | 时区，当前一期统一使用 `+08:00` |
| by_weekdays | array<string> | 可空 | 按周重复时指定星期几 |
| by_month_day | array<integer> | 可空 | 按月重复时指定每月第几天 |
| by_month | array<integer> | 可空 | 按年重复时指定月份 |
| by_work_day | boolean | 可空 | 是否支持“工作日”语义 |
| end_type | string | 枚举，非空 | 周期结束方式，`none` / `until` / `count` |
| end_at | datetime | 可空 | 当 `end_type=until` 时的结束时间 |
| count | integer | 可空，>= 1 | 当 `end_type=count` 时的执行次数 |

### 3.3 `timer_instance`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 实例唯一标识 |
| task_id | string | 外键，非空 | 所属定时任务 ID |
| schedule_id | string | 非空 | 所属日程 ID |
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

### 3.4 `reminder_rule`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 提醒规则唯一标识 |
| task_id | string | 外键，非空 | 所属定时任务 ID |
| schedule_id | string | 非空 | 所属日程 ID |
| reminder_type | string | 枚举，非空 | `weak` / `strong` |
| offset_minutes | integer | 非空 | 相对 occurrence 开始时间的偏移分钟 |
| enabled | boolean | 非空 | 是否启用 |
| can_snooze | boolean | 非空 | 是否允许推迟；弱提醒必须为 `false` |
| max_snooze_count | integer | 可空，>= 0 | 强提醒最大推迟次数 |
| snooze_interval_minutes | integer | 可空，> 0 | 强提醒默认推迟间隔 |
| channel | string | 可空 | 提醒渠道 |
| source | string | 枚举，非空 | `system_default` / `user_defined` |
| status | string | 枚举，非空 | `active` / `disabled` |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |

约束：
- 同一 `task_id` 下，只允许一条 `reminder_type=strong && offset_minutes=0` 的强提醒规则处于 `active`。
- 弱提醒规则允许多条并存，用于表达“默认前 10 分钟 + 用户额外前 30 分钟”等场景。

### 3.5 `reminder_trigger`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 提醒触发唯一标识 |
| reminder_rule_id | string | 外键，非空 | 来源提醒规则 ID |
| task_id | string | 外键，非空 | 所属定时任务 ID |
| instance_id | string | 外键，非空 | 所属 occurrence 实例 ID |
| schedule_id | string | 非空 | 所属日程 ID |
| reminder_type | string | 枚举，非空 | `weak` / `strong` |
| planned_trigger_at | datetime | 非空 | 按规则计算出的原始提醒时间 |
| actual_trigger_at | datetime | 非空 | 当前实际触发时间；snooze 后会变化 |
| status | string | 枚举，非空 | 提醒触发状态 |
| can_snooze | boolean | 非空 | 是否允许推迟 |
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

### 3.6 `calendar_view_occurrence`

> 非持久化查询视图对象，由服务端在查询时基于规则展开并叠加实例例外后返回。

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| occurrence_id | string | 非空 | occurrence 稳定标识，建议由 `task_id + planned_start_at` 派生 |
| task_id | string | 非空 | 所属定时任务 ID |
| schedule_id | string | 非空 | 所属日程 ID |
| instance_id | string | 可空 | 已物化实例的 ID；未物化时为空 |
| planned_start_at | datetime | 非空 | 基于规则展开得到的原始开始时间 |
| planned_end_at | datetime | 可空 | 基于规则展开得到的原始结束时间 |
| actual_trigger_at | datetime | 可空 | 叠加推迟或单次修改后的实际触发时间 |
| status | string | 枚举，非空 | 用户查询视图中的当前有效状态 |
| is_recurring | boolean | 非空 | 是否来自周期规则 |
| is_exception | boolean | 非空 | 是否叠加了实例级例外 |
| override_fields | object | 可空 | 该次 occurrence 的覆盖字段 |
| payload | object | 可空 | 上游展示所需字段快照，如标题、备注、地点等 |

约束：
- `calendar_view_occurrence` 仅作为查询返回结构，不参与提醒调度，不要求落库。
- 查询实现应先展开规则，再合并实例级例外；若两者冲突，以实例级例外为准。
