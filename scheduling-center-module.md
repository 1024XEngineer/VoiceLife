# 调度中心模块需求与设计文档 V2

本文档定义声活 VoiceLife 的调度中心。它不是为了把旧定时任务拆成更多对象，而是把“什么时候触发某种行为”从日程和动作中拿出来，保留最少的两类调度事实：触发规则与单次触发。

## 1. 行业调研及成熟方案

1. Google Calendar API: <https://developers.google.cn/workspace/calendar/api/concepts/events-calendars?hl=zh-cn>
2. iCalendar (RFC 5545): <https://icalendar.org/RFC-Specifications/iCalendar-RFC-5545/>
3. Microsoft Graph calendar resource: <https://learn.microsoft.com/en-us/graph/api/resources/calendar?view=graph-rest-1.0&preserve-view=true>

日历产品通常将用户意图、某次发生时间和通知投递分开。VoiceLife 的三模块边界应当服务于这个目标，而非复制完整的日历平台对象模型：

| 模块 | 权威实体 | 回答的问题 |
| --- | --- | --- |
| 日程 | `schedule`、`recurrence_rule`、日程例外 | 用户安排了什么，周期和某次改动是什么？ |
| 调度中心 | `trigger_rule`、`action_trigger` | 哪种行为何时触发？ |
| 动作 | `action_execution`、IM `delivery` | 触发后做了什么，IM 是否送达？ |

旧模型的迁移原则如下：

| 旧对象 | 新归属 | 简化后的处理 |
| --- | --- | --- |
| `timer_task` | 不再独立持有 | 日程与周期规则是日程模块事实；调度中心按需读取/缓存其版本，不创建第二套任务生命周期 |
| `timer_instance` | 不再独立持有 | 某次 occurrence 由日程模块展开；仅当需要触发动作时，用 `action_trigger.occurrence_at` 引用它 |
| `reminder_rule` | `trigger_rule` | 保留“提前多久触发”的必要配置 |
| `reminder_trigger` | `action_trigger` + `action_execution` | 前者保存时间和 snooze/dismiss，后者保存实际执行和投递 |

因此，`action_trigger` 不是又一种 Reminder Instance，而是旧 `reminder_trigger` 的调度部分。它和 `action_execution` 之间只有一次跨模块交接。

## 2. 核心目标

将日程模块已经确认的 occurrence 与简单的触发规则结合，在到期时发布一个可幂等消费的动作请求。

调度中心只解决 When：

- 使用日程模块拥有的开始时间、时区、周期规则和例外展开 occurrence。
- 用 `trigger_rule.offset_minutes` 计算某次 occurrence 的动作时间。
- 在需要 snooze、dismiss、取消或恢复时保存单次 `action_trigger`。
- 原子提交已发布触发和 `action_requested` 事件，保证崩溃恢复可以安全重放。

它不创建或修改日程，不保存日程的周期规则副本，不定义动作内容或渠道，也不记录 IM 投递和平台回执。

## 3. 核心概念定义

- **`schedule_id` 日程标识**
  - 日程模块拥有的业务意图引用。调度中心只引用，不能直接修改。
  - 跨模块以不透明标识传递；内部 ID 类型和 Adapter 映射不在本模块规定。

- **`schedule_revision` 日程版本**
  - 日程模块已确认变更的版本或等价事件标识。
  - 调度中心用它拒绝旧变更，并使已缓存的展开结果失效。

- **`occurrence_at` 日程发生时间**
  - 日程模块根据 `recurrence_rule` 和例外得到的一次实际 occurrence 时间。
  - 它不是调度中心的实体；调度中心只将其作为触发的稳定定位键。

- **`trigger_rule` 触发规则**
  - 属于一个日程的简单配置：什么动作在 occurrence 前/后多少分钟触发。
  - 当前 MVP 使用 `action_kind=reminder` 与 `reminder_level=weak/strong`；IM 是动作模块对提醒的渠道能力，不另建 IM 调度规则。
  - 周期由日程模块定义，偏移由本规则定义，两者不相互复制。

- **`action_trigger` 动作触发**
  - 针对一条 `trigger_rule` 和一次 `occurrence_at` 的单次调度事实。
  - 它保存计划/实际触发时间以及 `snooze`、`dismiss`、取消和过期状态。
  - 未发生单次操作时，未来触发可由规则按需计算；不要求为所有未来 occurrence 预建记录。

## 4. 核心业务流程

### 4.1 日程接入与规则维护

1. 日程模块在创建、修改、取消或恢复日程后，向调度中心发送 `schedule_id`、`schedule_revision` 和已确认的时间变化。
2. 调度中心只记录已处理版本，必要时使该日程未来的计算缓存失效；它不保存可编辑的周期规则副本。
3. 调用方以 `ReplaceTriggerRules` 一次提交一个日程当前完整的触发规则集，避免规则的增删改接口扩散。
4. 每条规则只说明动作种类、强弱等级和相对 occurrence 的偏移；内容、渠道、账户和凭据不是规则字段。
5. 规则变更只影响尚未发布的未来触发；已发生的 `action_trigger` 保留历史。

### 4.2 到期推进与动作请求

1. Runner 查询活动触发规则，并向日程模块请求近端时间窗内的 occurrence。
2. 调度中心以 `occurrence_at + offset_minutes` 计算到期时间；已有单次 `action_trigger` 覆盖优先于规则计算。
3. 到期时，模块创建或更新该次 `action_trigger` 为 `released`。
4. 在同一原子提交中写入稳定 `event_id` 的 `action_requested` 事件。
5. 动作模块可重复接收该事件，但按 `event_id` 只创建一条 `action_execution`；这提供幂等，而不承诺外部渠道的 exactly-once 投递。
6. 调度中心推进下一段扫描窗口，不为远期周期日程批量生成实例。

### 4.3 日程修改、例外与取消

1. 用户对“本次”“本次及以后”“全部”的修改，先由日程模块完成并形成新的 `schedule_revision`。
2. 日程模块在展开 occurrence 时已叠加本次例外；调度中心不再维护第二份 `timer_instance` 或例外模型。
3. 调度中心收到新版本后，取消或重算尚未发布、且受影响的未来 `action_trigger`。
4. 日程取消停止新的触发计算，并将尚未发布的单次触发标记为 `cancelled`。
5. 已发布动作的处理结果由动作模块保留；日程变更不得倒写它的投递或执行状态。

### 4.4 强提醒的稍后与关闭

1. 动作模块仅在强提醒的有效窗口内受理 `snooze` 或 `dismiss`，并生成带 `command_id` 的调度命令。
2. `SnoozeActionTrigger` 依据 `action_trigger_id` 将 `actual_trigger_at` 向后移动；重复 `command_id` 返回原结果。
3. `DismissActionTrigger` 使该次触发进入 `dismissed`，不影响日程本身或后续周期 occurrence。
4. 弱提醒不接受上述命令；参数、过期、终态和非强提醒请求必须明确拒绝。
5. snooze 后再次到期会为同一 `action_trigger` 发布新的 `event_id`，动作模块因而创建一次新的执行记录。

### 4.5 查询与恢复

1. 用户的“明天有什么安排”查询由日程模块负责；调度中心不提供第二个日历视图。
2. 调度中心仅查询触发历史或待处理触发，返回计划时间、实际时间、规则和调度状态。
3. IM 是否接受、送达、失败或重试由动作模块查询。
4. 服务重启后，调度中心扫描未终态触发和近端 occurrence；唯一键防止重复创建触发。
5. 任一渠道失败不回滚日程、规则或已发布的调度事实。

---

## 5. 模块接口

### 5.1 接口总览

| 接口 | 调用方 | 说明 |
| --- | --- | --- |
| ApplyScheduleChange | 日程模块 | 通知已确认的日程版本变化 |
| ReplaceTriggerRules | 日程应用层 | 整体替换一个日程的触发规则 |
| AdvanceDueTriggers | Runner | 扫描到期触发并发布动作请求 |
| SnoozeActionTrigger | 动作模块 | 推迟一次强提醒触发 |
| DismissActionTrigger | 动作模块 | 关闭一次强提醒触发 |
| ListActionTriggers | 查询层 | 查询调度事实，不承担日历查询 |

### 5.2 接口参数

#### 5.2.1 ApplyScheduleChange

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| request_id | string | 是 | 非空，幂等 | 日程变更事件标识 |
| schedule_id | string | 是 | 不透明引用 | 日程标识 |
| schedule_revision | string | 是 | 可比较的新版本 | 已确认日程版本 |
| change_type | string | 是 | `created` / `updated` / `cancelled` / `restored` | 变更类型 |
| effective_from | datetime | 否 | 周期局部变更时提供 | 重算起点 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| schedule_id | string | 非空 | 已处理日程 |
| applied_revision | string | 非空 | 当前已应用版本 |
| cancelled_trigger_count | integer | >= 0 | 受影响的未来未发布触发数 |

#### 5.2.2 ReplaceTriggerRules

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| request_id | string | 是 | 非空，幂等 | 规则集修改标识 |
| schedule_id | string | 是 | 活动日程 | 规则所属日程 |
| rules | array<object> | 是 | 可为空，表示关闭全部规则 | 当前完整规则集 |

`rules` 子项：

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| trigger_rule_id | string | 可空 | 更新已有规则时传入 |
| action_kind | string | 必填 | 当前仅 `reminder` | 目标动作 |
| reminder_level | string | 必填 | `weak` / `strong` | 提醒等级 |
| offset_minutes | integer | 必填 | 相对 occurrence 时间 | 提前/延后分钟数 |
| enabled | boolean | 必填 | 非空 | 是否继续派生未来触发 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| schedule_id | string | 非空 | 所属日程 |
| rules | array<object> | 可空 | 保存后的完整规则集 |

约束：同一日程最多一条启用的 `strong` 且 `offset_minutes=0` 规则；弱提醒规则不得携带交互配置。

#### 5.2.3 AdvanceDueTriggers

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| now | datetime | 是 | ISO 8601 | 本次到期边界 |
| limit | integer | 否 | 1 到 1000 | 最多处理规则/触发数 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| released_trigger_count | integer | >= 0 | 已发布触发数 |
| emitted_event_count | integer | >= 0 | 动作请求事件数 |

#### 5.2.4 SnoozeActionTrigger

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| command_id | string | 是 | 非空，幂等 | 动作模块命令标识 |
| action_trigger_id | string | 是 | 非空 | 要推迟的触发 |
| delay_minutes | integer | 是 | 大于 0 | 推迟时长 |
| requested_at | datetime | 是 | ISO 8601 | 命令受理时间 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| action_trigger_id | string | 非空 | 已重排触发 |
| actual_trigger_at | datetime | 非空 | 新的触发时间 |
| snooze_count | integer | >= 0 | 累计推迟次数 |

#### 5.2.5 DismissActionTrigger

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| command_id | string | 是 | 非空，幂等 | 动作模块命令标识 |
| action_trigger_id | string | 是 | 非空 | 要关闭的触发 |
| requested_at | datetime | 是 | ISO 8601 | 命令受理时间 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| action_trigger_id | string | 非空 | 已关闭触发 |
| status | string | `dismissed` | 当前终态 |

#### 5.2.6 ListActionTriggers

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| schedule_id | string | 否 | 可为空 | 日程过滤 |
| trigger_rule_id | string | 否 | 可为空 | 规则过滤 |
| status | string | 否 | 触发状态枚举 | 状态过滤 |
| range_start | datetime | 否 | 与 `range_end` 成对提供 | 实际触发范围起点 |
| range_end | datetime | 否 | 大于 `range_start` | 实际触发范围终点 |
| page | integer | 否 | >= 1 | 页码 |
| page_size | integer | 否 | 1 到 100 | 每页数量 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| action_triggers | array<object> | 可空 | 触发事实列表 |
| total | integer | >= 0 | 总数 |
| has_more | boolean | 非空 | 是否还有下一页 |

### 5.3 下游契约

调度中心向动作模块发布 `action_requested`。它只表示到期，不代表动作已执行或消息已送达。

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| event_id | string | 是 | 唯一且稳定，用于幂等消费 |
| event_type | string | 是 | 固定为 `action_requested` |
| occurred_at | datetime | 是 | 事件提交时间 |
| action_trigger_id | string | 是 | 本次触发 |
| schedule_id | string | 是 | 关联日程 |
| occurrence_at | datetime | 是 | 所属日程 occurrence |
| action_kind | string | 是 | 当前为 `reminder` |
| reminder_level | string | 是 | `weak` / `strong` |
| trigger_at | datetime | 是 | 本次实际触发时间 |
| context | object | 否 | 动作所需的日程内容快照 |

约束：

- `event_id` 与 `action_trigger` 的一次 `released` 转换原子提交。
- 动作模块按 `event_id` 幂等受理；相同标识但不同内容必须拒绝。
- 调度中心不等待动作执行或渠道投递结果。

### 5.4 状态约定

- `trigger_rule.status`
  - `active`：继续从日程 occurrence 派生未来触发。
  - `disabled`：不再产生未来触发。

- `action_trigger.status`
  - 非终态：`pending`、`released`、`snoozed`。
  - 终态：`dismissed`、`cancelled`、`expired`。
  - 允许流转：`pending -> released / cancelled`，`released -> snoozed / dismissed / expired`，`snoozed -> released / dismissed / expired`。
  - `released` 仅表示已发布动作请求；投递与送达状态属于动作模块。

## 6. 主要数据模型

### 6.1 `trigger_rule`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 触发规则标识 |
| schedule_id | string | 外键，非空 | 所属日程 |
| action_kind | string | 非空，枚举 | 当前仅 `reminder` |
| reminder_level | string | 非空，枚举 | `weak` / `strong` |
| offset_minutes | integer | 非空 | 相对 occurrence 的触发偏移 |
| status | string | 非空，枚举 | `active` / `disabled` |
| last_schedule_revision | string | 非空 | 最近处理的日程版本 |
| next_occurrence_at | datetime | 可空 | 仅为扫描效率保存的游标，不是 occurrence 实体 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后更新时间 |

### 6.2 `action_trigger`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 动作触发标识 |
| trigger_rule_id | string | 外键，非空 | 来源触发规则 |
| schedule_id | string | 非空 | 所属日程 |
| occurrence_at | datetime | 非空 | 日程模块展开的 occurrence 时间 |
| planned_trigger_at | datetime | 非空 | 规则计算出的时间 |
| actual_trigger_at | datetime | 非空 | 当前实际触发时间 |
| status | string | 非空，枚举 | 调度状态 |
| snooze_count | integer | 非空，>= 0 | 累计推迟次数 |
| last_event_id | string | 可空，唯一 | 最近一次发布的动作请求 |
| created_at | datetime | 非空 | 创建时间 |
| updated_at | datetime | 非空 | 最后更新时间 |

约束：同一 `trigger_rule_id + occurrence_at` 最多一条未删除触发。snooze 后再次发布必须生成新的 `event_id`，但仍复用同一 `action_trigger_id`。
