# 定时任务模块需求与设计文档 V1

本文档旨在定义 声活 VoiceLife 的定时任务模块。该模块为 App 的核心调度层模块，我参考了成熟解决方案 iCalendar 规范以及热门产品 Google Calendar、Microsoft Outlook 的 API 文档，分析其字段，理解其具体实现，结合自身业务场景，提出一套模块需求与设计文档。


## 1. 行业调研及成熟方案

1. Google Calendar API Document <https://developers.google.cn/workspace/calendar/api/concepts/events-calendars?hl=zh-cn>
2. iCalendar(RFC 5545) <https://icalendar.org/RFC-Specifications/iCalendar-RFC-5545/>
3. Microsoft Outlook <https://learn.microsoft.com/en-us/graph/api/resources/calendar?view=graph-rest-1.0&preserve-view=true>

根据调研结果，在对象处理方面，各家产品均做到了业务模型、调度模型、执行模型分离。与本模块的主要业务对象对应关系如下表：

| 本模块业务对象      | Google Calendar   | Outlook       | RFC5545       |
| --------- | ----------------- | ------------- |  ------------- |
| Schedule<br/>业务模型  | Event             | Event         | VEVENT        |
| TimerTask<br/>调度模型 | Master Event | Series Master | RRULE         |
| Instance<br/>执行模型  | Instance          | Occurrence    | RECURRENCE-ID |

## 2. 核心目标

将用户输入的日程数据转化为调度层数据，旨在规划每一个事件的 recurrence，使得系统按照用户意图将单一或周期性事件实例化，让每一实例均可以被精确改动与触发。

## 3. 核心概念定义

定义系统核心实体，以更精确地匹配业务模型。三层拆分关系如下：

- **`schedule_id`** **日程标识**
  - 对应上游模块的日程实体，是用户业务意图的引用。
  - 本模块仅读取与转发，不负责维护其业务状态。
  - 解决 What 的问题，即“用户想做什么”。
  
- **`task_id`** **定时任务标识**
  - 模块核心实体，由 `schedule_id` 派生而来。
  - 承载具体的调度策略与参数，如 `next_trigger_at`、`recurrence_rule` 等。
  - 负责把一条日程转换成可执行的调度任务，解决“系统应该如何安排触发”的问题。

- **`instance_id`** **实例标识**
  - 由定时任务生成的单次执行实体，代表某一个具体触发时刻。
  - 它是用户最终对于单次 recurrence 的操作对象，适合做单次改动、例外处理和触发确认。
  - 解决“这一次具体触发是什么”的问题。


## 3. 核心业务流程

系统的运作主要分为三大流程：

### 流程一：上游日程接入与任务注册

1. 上游日程模块创建或更新一条 `schedule`，并将日程内容、开始时间、循环规则等传入本模块。
2. 本模块以 `schedule_id` 为引用，创建或更新对应的 `timer_task`。
3. `timer_task` 保存调度所需的核心字段，如 `next_trigger_at`、`recurrence_rule`、`effective_from` 等。
4. 对于一次性日程，任务通常只派生一个实例；对于周期日程，任务会持续维护下一次触发时间。

### 流程二：实例生成与触发执行

1. 当 `timer_task` 到达 `next_trigger_at` 时，系统生成对应的 `timer_instance`。
2. `timer_instance` 是一次具体触发的执行对象，负责承接本次提醒、通知或动作执行。
3. 执行完成后，系统根据规则推进任务的下一次触发时间，并生成后续实例。
4. 如果本次执行失败，系统只回写本次实例状态，不直接破坏整条任务链路。

### 流程三：单次改动、例外处理与重算

1. 用户如果只修改“本次”，通常只影响某一个 `timer_instance`，不改整条 `timer_task`。
2. 用户如果修改“本次及以后”，系统以 `effective_from` 为边界，重算后续的调度规则与实例。
3. 用户如果取消日程，本模块会停止后续实例生成，并将对应任务标记为终止态。
4. 整个过程中，`schedule` 作为上游业务意图，`timer_task` 作为调度规则载体，`timer_instance` 作为最终执行结果，三者职责分离、单向影响。


---

# 定时任务模块接口设计文档

## 2. 模块接口

基本关系：`Schedule` 日程(上游, What) -> `TimerTask` 任务系列(具体到 Title，如每周六提醒的吃药) -> `Instance`(具体到时间，如系列提醒中 7 月 25 日周六的吃药)
- `schedule`：日程模块中的业务记录，表示“用户想在什么时间做什么事”。
- `timer_task`：定时任务模块中的调度记录，表示“系统根据这条日程，应该如何安排后续触发”。
- `timer_instance`：某一次具体触发实例，表示“这一次提醒本身”。
- 上游 `schedule` 的状态遵循 `active` / `completed` / `cancelled`，本模块只读取不维护。

约束:
- 一条 `schedule` 可以对应一条 `timer_task`。
- 一条 `timer_task` 可以派生出一条或多条 `timer_instance`。
- 一次性日程通常对应 1 个实例。
- 周期日程通常会不断生成后续实例，但一般只维护最近一次或一个较小时间窗口内的实例。
- 当用户修改“单次”时，通常是对某个 `timer_instance` 做例外处理，不直接改变整条 `timer_task` 的周期规则。
- 当用户修改“本次及以后”时，需要以 `effective_from` 为边界，重算该时间点之后的任务和实例。
- `change_scope` 语义如下：
  - `single`：仅作用于某一个 `timer_instance`。
  - `future`：从 `effective_from` 开始，作用于后续未终态实例及后续调度规则。
  - `all`：作用于整个 `timer_task` 系列；历史已终态实例保留，未终态实例与后续规则按新配置重算。
- `UpdateTimerTask` 的 `all` 场景会保留历史终态实例，仅重算当前未终态实例与后续规则。
- `CancelTimerTask` 的 `all` 场景会将 `timer_task` 置为 `terminated`，并将所有未终态实例标记为 `skipped`，历史终态实例保留。
- `GenerateInstances` 需保证实例级幂等，同一 `task_id` 下相同 `planned_at` 不应重复生成。
- `RegisterTimerTask` 对同一 `schedule_id` 采用幂等 upsert，已存在时更新原 `timer_task`，不重复创建。
- `taskId` 为主标识，`schedule_id` 为辅助来源字段；若传入则必须与 `taskId` 绑定记录一致，未传则按 `taskId` 处理。
- 所有时间字段统一使用 ISO 8601 表示，实例生成与重算优先采用 `recurrence_rule.timezone`，未配置时默认 `+08:00`。

三者为上下游关系，影响自上而下，且不可反向影响。

### 2.1 接口总览

| 接口 | Method | Path | 说明 |
| --- | --- | --- | --- |
| RegisterTimerTask | POST | `/v1/timer-tasks` | 注册定时任务 |
| UpdateTimerTask | PATCH | `/v1/timer-tasks/{taskId}` | 更新定时任务 |
| CancelTimerTask | DELETE | `/v1/timer-tasks/{taskId}` | 取消定时任务 |
| GenerateInstances | POST | `/v1/timer-tasks/{taskId}/instances` | 基于任务生成实例 |
| ListInstances | GET | `/v1/timer-instances` | 查询实例列表 |
| SnoozeInstance | POST | `/v1/timer-instances/{instanceId}/snooze` | 推迟实例 |
| DismissInstance | POST | `/v1/timer-instances/{instanceId}/dismiss` | 关闭实例 |

### 2.2 接口参数

#### 2.2.1 RegisterTimerTask

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| schedule_id | string | body | 是 | 来源于上游 schedule | 日程 ID |
| start_at | datetime | body | 是 | ISO 8601 | 首次触发时间 |
| recurrence_rule | object | body | 否 | 一次性日程可为空 | 周期规则 |
| reminder_config | object | body | 否 | 具体结构由提醒能力定义 | 提醒配置 |

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
| reminder_config | object | body | 否 | 具体结构由提醒能力定义 | 更新后的提醒配置 |
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

#### 2.2.5 ListInstances

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

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| instances | array<object> | 可空 | 符合条件的实例列表 |
| total | integer | 大于等于 0 | 实例总数 |
| page | integer | 大于 0 | 当前页码 |
| page_size | integer | 大于 0 | 每页数量 |
| has_more | boolean | 非空 | 是否还有下一页 |

#### 2.2.6 SnoozeInstance

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| instanceId | string | path | 是 | 路径参数 | 实例 ID |
| delay_minutes | integer | body | 是 | 大于 0 | 推迟时长，单位分钟 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| instance_id | string | 唯一 | 被推迟的实例 ID |
| status | string | 枚举 | 通常为 `snoozed` |
| trigger_at | datetime | 可空 | 推迟后的实际触发时间 |
| delay_count | integer | 大于等于 0 | 推迟后的累计次数 |

#### 2.2.7 DismissInstance

**请求参数**

| 参数名 | 类型 | 位置 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- | --- |
| instanceId | string | path | 是 | 路径参数 | 实例 ID |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| instance_id | string | 唯一 | 被关闭的实例 ID |
| status | string | 枚举 | 通常为 `dismissed` |

### 2.3 状态约定

- `timer_task.status`
  - `active`：运行中。
  - `paused`：暂停中，恢复前不生成新实例。
  - `terminated`：终止态，不再生成新实例。
- `timer_instance.status`
  - 非终态：`pending`、`snoozed`、`modified`。
  - 终态：`triggered`、`dismissed`、`skipped`。
  - 允许流转：
    - `pending -> snoozed / modified / triggered / dismissed / skipped`
    - `snoozed -> snoozed / modified / triggered / dismissed / skipped`
    - `modified -> triggered / dismissed / skipped`
    - `triggered -> dismissed`
  - `dismissed`、`skipped` 为终态，不再回退。


## 3. 数据模型

### 3.1 `timer_task`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 定时任务唯一标识 |
| schedule_id | string | 外键，非空 | 关联的日程 ID |
| status | string | 枚举，非空 | 任务状态，`active` / `paused` / `terminated` |
| next_trigger_at | datetime | 可空 | 下一次预计触发时间 |
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
| planned_at | datetime | 非空 | 按规则计算出的原始计划触发时间 |
| trigger_at | datetime | 非空 | 实际用于触发提醒的时间 |
| status | string | 枚举，非空 | 实例状态，支持 `pending` / `triggered` / `snoozed` / `dismissed` / `skipped` / `modified` |
| delay_count | integer | 非空，>= 0 | 当前实例已被推迟的次数 |
| override_fields | object | 可空 | 本次实例相对原规则的覆盖字段 |
| last_action_at | datetime | 可空 | 最后一次用户操作或系统状态变更时间 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后一次更新时间 |
| deleted_at | datetime | 可空 | 软删除时间，`NULL` 表示未删除 |
