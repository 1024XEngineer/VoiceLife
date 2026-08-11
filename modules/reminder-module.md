# 提醒触发器模块需求与设计文档 V3

本文档定义 VoiceLife 的提醒触发器，解决日程到期后的提醒触发功能。日程负责用户安排及周期规则，提醒触发器负责某个时刻是否应触发，语音提醒和 IM 负责输出。

## 1. 行业调研及成熟方案

1. Google Calendar API: <https://developers.google.cn/workspace/calendar/api/concepts/events-calendars?hl=zh-cn>
2. iCalendar (RFC 5545): <https://icalendar.org/RFC-Specifications/iCalendar-RFC-5545/>
3. Microsoft Graph calendar resource: <https://learn.microsoft.com/en-us/graph/api/resources/calendar?view=graph-rest-1.0&preserve-view=true>
4. ESP平台定时支持 <https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/system/esp_timer.html>

成熟日历系统会把用户意图、周期展开和实际通知分开。VoiceLife 只保留完成当前设备侧提醒所需的最小边界：

| 边界 | 权威事实 | 回答的问题 |
| --- | --- | --- |
| 日程模块 | `schedule`、`recurrence_rule`、日程例外 | 用户安排了什么；某个日期有哪些 occurrence？ |
| 提醒触发器 | `trigger`、`notification_outbox` | 哪个具体时刻需要输出；该输出是否需要重试？ |
| 语音提醒与 IM | 各自的输出 Port 和 Adapter | 如何把已到期的内容播放或投递出去？ |


## 2. 核心目标

提醒触发器通过两个入口工作：`RegisterTrigger` 注册或改变触发，`RunDueTriggers` 由 Runner 在到点时推进。Runner 在本地事务内确认触发并写入 Outbox，提交后直接调用语音提醒和 IM Port。

流程：

- 向日程模块查询某日程下一次有效 occurrence，并按偏移计算具体 `trigger_at`。
- 保存尚未完成的具体 `trigger`，接受取消、稍后和关闭等单次运行态改变。
- 写入 `trigger` 状态和每个输出目标的 `notification_outbox`，在重启或调用失败后安全重放。
- 计算下一次唤醒时间，让硬件定时器只在需要时 esp_timer 唤醒 Runner。

**提醒触发器不创建或修改日程，不保存 RRULE、时区或日程例外的副本，不提供“明天有什么安排”的查询，也不直接调用 HTTP、IM SDK、设备驱动或语音 SDK。**

## 3. 核心概念定义

- **`schedule_id` 日程标识** 
  - 上游
  - 日程模块拥有的业务意图引用；提醒触发器只能引用，不能修改。

- **`occurrence_at` 日程发生时间**
  - 上游
  - 日程模块依据 RRULE、时区和例外得出的某次实际发生时间。
  - 它不是提醒触发器实体；提醒触发器只在 `trigger` 中引用它作为稳定定位键。

- **`trigger` 触发**
  - 提醒触发器唯一的业务实体：某条日程的某次 `occurrence_at`，应在 `trigger_at` 向一个或多个输出目标发出提醒。
  - 创建时只物化下一次需要处理的 occurrence；周期日程在该触发被释放后，才由 Runner 向日程模块请求下一次并原子注册新的触发（多条提醒则注册多条）。
  - `snooze` 和 `dismiss` 只作用于该条 `trigger`，不修改日程及后续 occurrence。

- **`notification_outbox` 通知发件箱**
  - `trigger` 释放时与其原子写入的可靠投递记录，每个输出目标一条。
  - 它是技术可靠性事实，不是动作模块或 Reminder Instance；记录只说明某个 Port 是否仍需调用，不声称用户一定已看到通知。

- **`ReminderOutputPort`、`ImNotificationPort` 输出 Port**
  - 下游
  - Runner 提交 Outbox 后调用的两个边界。Port 必须以 `delivery_id` 幂等：重复调用不得重复创建不可接受的外部效果。
  - 具体语音设备、IM 平台、HTTP、SDK、凭据和回执由各 Adapter 负责，不能进入 Runner 业务逻辑。

## 4. 核心业务流程

### 4.1 注册与日程变更

1. 日程创建、修改、取消或恢复完成后，应用层调用 `RegisterTrigger`，提交 `schedule_id` 和当前提醒配置。
2. 提醒触发器通过日程查询 Port 获取下一次有效 `occurrence_at`，按 `reminders` 中每项的 `offset_minutes` 算出各自的 `trigger_at`，为下一次 occurrence 保存对应的 `pending` 触发（多条提醒则保存多条），并将该 `reminders` 配置快照写入该日程的 `reminder_config`。
3. 日程版本变化或整条规则取消时，尚未释放的旧触发进入 `cancelled`；已写入的 Outbox 记录不因日程变更而修改。每次新的释放（含 snooze 后再次触发）都会重新查询日程模块，生成当前内容的新快照。
4. 用户的“本次”“本次及以后”“全部”修改先由日程模块形成例外和新版本，再由本入口重新计算触发；提醒触发器不解释 RRULE 或例外范围。

### 4.2 到期推进与直接输出

1. 定时器唤醒、设备启动、日程注册完成或 Outbox 重试到期时，Runtime 调用 `RunDueTriggers(now)`。
2. Runner 取得 `trigger_at <= now` 的活动触发；对每条触发原子地标记为 `released`，并为 `voice_reminder`、`im_notification` 等目标写入稳定 `delivery_id` 的 Outbox 记录。
3. 对周期日程，Runner 在同一事务内向日程模块请求下一次有效 occurrence，并按该日程 `reminder_config` 中的当前提醒配置注册新的 `pending` 触发（多条提醒则注册多条）；无下一次 occurrence 时跳过。释放、写 Outbox 与注册下一次必须同一事务原子提交。
4. 本地事务提交后，Runner 依次读取待投递 Outbox，通过 `ReminderOutputPort` 和 `ImNotificationPort` 输出。
5. Port 调用成功后，Outbox 标记为 `delivered`；调用失败则按退避策略标记为 `retry_pending`，下次 Runner 重试同一 `delivery_id`。

### 4.3 稍后、关闭与恢复

1. IM 侧用户动作经服务端验签与幂等校验后，在强提醒有效窗口内通过临时 SSE 下发 `ReminderActionCommand`；本地 `ImActionChannel` 接收命令（`operationId` 幂等、`expiresAt` 窗口校验）后调用 `RegisterTrigger`，以 `operation=snooze` 或 `operation=dismiss` 提交。设备本地语音交互入口同样直接调用 `RegisterTrigger`。弱提醒不建立动作流。
2. `snooze` 只接受仍可交互的强提醒，将该 `trigger.trigger_at` 延后，并递增其 `delivery_sequence`；下一次到期产生新的 Outbox 记录。
3. `snooze` 生效时，取消该 `trigger` 下尚未送达的旧 Outbox 记录（`status` 为 `pending` 或 `retry_pending`），仅保留新 `delivery_sequence` 产生的记录，避免已推迟的提醒继续重试打扰用户。
4. `dismiss` 将该次 `trigger` 标为 `dismissed`，不影响日程和后续周期触发。弱提醒、已取消或终态触发必须拒绝这些操作（返回 `rejected`）。
5. 设备重启后，Runner 从持久化的活动触发和待投递 Outbox 恢复；任一输出渠道失败不回滚日程或另一输出渠道的结果。

### 4.5 注释 - 硬件定时与唤醒

1. Runtime 从持久化 `trigger.trigger_at` 与 Outbox 的下次重试时间中选出最近时间，使用 `esp_timer` 设置一次性唤醒。
2. `esp_timer` 回调只通知或唤醒 Runner；它不展开 RRULE、不改变触发状态，也不调用语音或 IM Port。

---

## 5. 模块接口

### 5.1 接口总览

| 接口 | 调用方 | 说明 |
| --- | --- | --- |
| `RegisterTrigger` | 日程应用层、本地 `ImActionChannel`（IM 动作命令）、语音交互入口 | 统一注册、更新、取消、稍后或关闭触发的命令入口 |
| `RunDueTriggers` | Runtime Runner | 推进到期触发与待投递 Outbox；不是面向业务调用方的查询 API |


### 5.2 接口参数

#### 5.2.1 `RegisterTrigger`

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| `request_id` | string | 是 | 非空，幂等 | 注册或变更请求标识 |
| `operation` | string | 是 | `upsert` / `cancel` / `snooze` / `dismiss` | 统一命令类型 |
| `trigger_id` | string | 条件必填 | `cancel`、`snooze`、`dismiss` 必填 | 已注册触发标识 |
| `schedule_id` | string | `upsert` 必填 | 不透明引用 | 所属日程 |
| `reminders` | array<object> | `upsert` 必填 | 非空；每项 `{ offset_minutes, reminder_level, output_targets }`，`reminder_level` 为 `weak` / `strong`，`output_targets` 为 `voice_reminder`、`im_notification` 的非空子集 | 该日程当前生效的提醒配置；可多项，各自物化为独立 `trigger` |
| `scope` | string | `cancel` 可选 | `occurrence` / `series`，默认 `occurrence` | 取消范围 |
| `delay_minutes` | integer | `snooze` 必填 | 大于 0 | 推迟时长 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `trigger_ids` | array<string> | 非空 | 本次操作新建或更新的触发标识 |
| `result` | string | `registered` / `updated` / `cancelled` / `snoozed` / `dismissed` / `duplicate` / `rejected` | 命令结果 |

约束：`upsert` 只能引用日程模块已确认的版本；`snooze` 与 `dismiss` 不接受日程内容、RRULE 或输出凭据。配置变化只影响尚未释放的未来触发。
`upsert` 为全量同步语义：以提交的 `reminders` 数组作为该日程当前生效配置，覆盖保存到 `reminder_config`；对尚未释放的旧触发按 `(schedule_id, offset_minutes)` 定位——仍存在于数组中的更新（重新计算 `trigger_at`），不再出现的标记 `cancelled`；随后按新配置物化下一次 occurrence 的 `pending` 触发。`snooze` 或 `dismiss` 遇到弱提醒、已取消或终态触发时返回 `rejected`。

#### 5.2.2 `RunDueTriggers`

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| `now` | datetime | 是 | ISO 8601 | 本轮到期边界 |
| `limit` | integer | 否 | 1 到 1000 | 本轮最多推进的触发与 Outbox 记录数 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `released_trigger_count` | integer | >= 0 | 已释放的到期触发数 |
| `delivered_count` | integer | >= 0 | 本轮成功确认的 Outbox 投递数 |
| `retry_pending_count` | integer | >= 0 | 仍待重试的 Outbox 数 |
| `next_wake_at` | datetime | 可空 | Runtime 应设置的最近唤醒时间 |

### 5.3 内部 Port 契约

Runner 仅依赖以下 Port，不接触外部 SDK：

| Port | 输入 | 关键约束 |
| --- | --- | --- |
| `ScheduleQueryPort` | `schedule_id`、时间边界或 `occurrence_at` | 返回日程模块已经叠加 RRULE、时区和例外后的 occurrence；无下一次 occurrence 时返回空。 |
| `ReminderOutputPort` | `delivery_id`、日程内容快照、提醒等级 | 同一 `delivery_id` 必须幂等。 |
| `ImNotificationPort` | `delivery_id`、日程内容快照、提醒等级 | 同一 `delivery_id` 必须幂等；平台回执和凭据留在 IM Adapter。 |

### 5.4 状态约定

- `trigger.status`
  - 非终态：`pending`、`snoozed`、`released`。
  - 终态：`completed`、`dismissed`、`cancelled`、`expired`。
  - 允许流转：`pending -> released / cancelled`，`released -> completed / snoozed / dismissed / expired`，`snoozed -> released / dismissed / expired`。
  - `released` 仅表示已写入 Outbox；各输出的完成情况只由 Outbox 状态表达。
  - 该触发的全部 Outbox 记录进入终态（`delivered` 或 `failed`）后，`trigger` 进入 `completed`。

- `notification_outbox.status`
  - 非终态：`pending`、`processing`、`retry_pending`。
  - 终态：`delivered`、`failed`。
  - Outbox 达到重试上限可进入 `failed`，但不得回写或恢复已释放的 `trigger`。

## 6. 主要数据模型

本地存储必须支持事务：`trigger` 状态变更与其 Outbox 写入在同一事务内原子提交，这是"释放-重放"可靠性的前提。

### 6.1 `trigger`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `id` | string | 主键，唯一，非空 | 触发标识 |
| `schedule_id` | string | 非空 | 日程引用 |
| `occurrence_at` | datetime | 非空 | 日程模块展开的具体 occurrence |
| `trigger_at` | datetime | 非空 | 当前实际到期时间；snooze 后改变 |
| `offset_minutes` | integer | 非空 | 相对 occurrence 的偏移 |
| `reminder_level` | string | 非空，枚举 | `weak` / `strong` |
| `output_targets` | array<string> | 非空 | 要投递到的 Port 集合 |
| `delivery_sequence` | integer | 非空，>= 0, <=3 | 同一触发第几次释放；snooze 后递增 |
| `status` | string | 非空，枚举 | 提醒触发器状态 |
| `created_at` | datetime | 非空 | 创建时间 |
| `updated_at` | datetime | 非空 | 最后更新时间 |


### 6.2 `reminder_config`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `schedule_id` | string | 主键，唯一，非空 | 日程引用 |
| `reminders` | json | 非空 | 当前生效提醒配置快照，结构同 `RegisterTrigger(upsert)` 的 `reminders` 入参 |
| `updated_at` | datetime | 非空 | 最后更新时间 |

约束：`upsert` 全量覆盖该配置；Runner 释放周期日程的触发后，据此注册下一次 occurrence 的触发集合。

### 6.3 `notification_outbox`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `delivery_id` | string | 唯一，非空 | Outbox 记录标识 |
| `trigger_id` | string | 外键，非空 | 来源触发 |
| `delivery_sequence` | integer | 非空，>= 0 | 对应触发的释放序号 |
| `target` | string | 非空，枚举 | `voice_reminder` / `im_notification` |
| `payload_snapshot` | object | 非空，不含凭据 | 输出所需日程上下文快照 |
| `status` | string | 非空，枚举 | 投递状态 |
| `attempt_count` | integer | 非空，>= 0 | 已尝试次数 |
| `next_attempt_at` | datetime | 可空 | 重试时间 |

约束：`trigger` 变为 `released` 与其目标集合的 Outbox 记录必须原子提交；同一 `trigger_id + delivery_sequence + target` 最多一条记录。Port 的调用发生在提交之后，重复调用依赖 `delivery_id` 幂等。`delivered` 表示云端已受理（IM 返回 202 / `NotificationSubmission`）；平台级送达回执与平台投递重试由 IM 服务端负责，本地不追踪。
