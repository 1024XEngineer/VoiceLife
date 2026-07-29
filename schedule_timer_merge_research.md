# 日程管理与定时任务模块是否合并调研

调研辅助：Codex
更新时间：2026-07-29

## 1. 调研目标

结合 VoiceLife 当前两份设计文档，重点调研 iCalendar、Google Calendar、Outlook 三套成熟模型，回答一个具体问题：

- 在模块设计层面，`日程管理` 与 `定时任务` 是否应该合并？

这里的“合并”需要拆成两层来看：

- 业务模型层是否合并：是否把日程数据管理与调度数据管理放进同一个模块。
- 能力实现层是否合并：是否把周期展开、提醒触发、推迟、关闭、回执发送都塞进同一个模块。

本文结论先行：

- **不推荐把日程管理与定时任务合并为一个模块。**
- 更准确地说，**建议保留“日程管理模块”与“定时任务模块”两条清晰边界，并通过明确契约衔接**。
- 当前 `schedule -> timer_task -> instance` 的三层认知是对的，`timer_task` 继续作为独立调度对象保留是合理的。

## 2. VoiceLife 当前设计现状

根据现有文档，当前方案已经形成了两套模块边界：

- 日程管理模块：负责 `schedule` 的创建、查询、修改、删除，是纯数据管理模块，不承载提醒执行。
- 定时任务模块：负责把 `schedule` 转成 `timer_task`，再生成 `instance`，承载周期、触发、推迟、关闭、回执。

当前方案的优点：

- 把“用户想做什么”与“系统如何触发”分开，概念清晰。
- 能较好支撑单次修改、未来修改、整系列修改。
- 为推迟、关闭、失败重试、幂等等运行时问题留出了空间。

当前方案的主要风险：

- 如果模块边界定义不严，`schedule` 与 `timer_task` 之间会出现双写和状态漂移。
- 如果修改范围语义没有统一，`single/future/all` 可能在两个模块里出现口径分叉。
- 如果调用链设计不好，日程侧可能需要感知太多调度细节，增加接入成本。

但这些风险本质上是**边界与契约设计问题**，并不意味着两个模块必须合并。

## 3. 三家方案调研

### 3.1 iCalendar：业务对象与提醒能力同属日历对象，但不定义独立调度模块

iCalendar RFC 5545 是最底层、最通用的日历交换模型。它有几个关键点：

- iCalendar 定义的是**日历数据格式**，而不是执行引擎。
- 它原生区分 `VEVENT` 和 `VTODO`，说明“事件”和“待办”在概念上并不完全等价。
- `VEVENT`、`VTODO` 都可以携带周期规则，也都可以挂接 `VALARM`。
- 周期实例的单次例外通过 `RECURRENCE-ID` 建模。

从 RFC 文本看：

- RFC 5545 将 iCalendar 定义为表示 `events`、`to-dos`、`journal entries`、`free/busy` 的通用格式，而非某个具体服务的调度实现。[RFC 5545](https://www.rfc-editor.org/rfc/rfc5545)
- `VEVENT` 可表示普通事件，也可表示周年纪念、每日提醒等日历项。[RFC 5545](https://www.rfc-editor.org/rfc/rfc5545)
- `VTODO` 表示 action-item / assignment，即待办或指派事项。[RFC 5545](https://www.rfc-editor.org/rfc/rfc5545)
- `RECURRENCE-ID` 用于标识 recurring event / to-do / journal 的某一个特定实例。[RFC 5545](https://www.rfc-editor.org/rfc/rfc5545)

这说明 iCalendar 的设计哲学是：

- **业务对象层**：事件和待办是第一层对象。
- **共享能力层**：周期规则、例外、提醒是附着在对象上的通用能力。
- **执行层**：标准本身不强行定义一个可见的 `timer_task` 资源。

对 VoiceLife 的启发：

- 你可以保留“源对象 + 周期规则 + 实例例外”的三层思想。
- iCalendar 没有要求一定暴露独立调度资源，但也没有否定实现侧把调度层单独建模。
- 对 VoiceLife 这种需要明确提醒执行、推迟、关闭、回执的产品，独立调度对象反而更利于落地。

### 3.2 Google Calendar：事件与任务分离，但 Calendar UI 做了统一呈现

Google 的模型比 iCalendar 更接近产品实现，可分成两套对象：

- Google Calendar `Event`
- Google Tasks `Task`

#### 3.2.1 Event 模型

Google Calendar 的 `events` 资源直接包含：

- `start`
- `end`
- `recurrence`
- `recurringEventId`
- `originalStartTime`
- `reminders`

官方文档表明：

- `events` 资源直接承载 `recurrence` 字段，使用 RRULE 字符串数组表达周期。[Events](https://developers.google.com/workspace/calendar/api/v3/reference/events)
- 事件级提醒通过 `reminders.overrides[]` 配置，而不是单独创建一个 reminder 资源。[Events](https://developers.google.com/workspace/calendar/api/v3/reference/events)
- 周期实例可通过 `events.instances()` 获取。[Recurring events](https://developers.google.com/workspace/calendar/api/guides/recurringevents)
- 单次实例不再携带 `recurrence`，而是通过 `recurringEventId` 和 `originalStartTime` 回指系列及原始发生时刻。[Recurring events](https://developers.google.com/workspace/calendar/api/guides/recurringevents)
- “本次及以后”修改不是更新一个隐藏任务，而是**把原系列截断，再新建一个后半段系列**。[Recurring events](https://developers.google.com/workspace/calendar/api/guides/recurringevents)

#### 3.2.2 Task 模型

Google Tasks API 的 `task` 是另一套资源，典型字段包括：

- `title`
- `notes`
- `due`
- `completed`
- `parent`
- `status`

官方 Tasks REST 资源页中并没有直接给出 `recurrence` 字段。[Google Tasks REST Resource](https://developers.google.com/workspace/tasks/reference/rest/v1/tasks)

但 Google Tasks 帮助中心又明确说明：

- Google Tasks 和 Google Calendar UI 都支持 repeating tasks。
- Calendar 网格中只显示有限数量的未来 recurring tasks，随着时间推进再自动出现新的任务实例。
- 修改范围支持 `This task`、`All tasks`，删除时还支持 `This and following tasks`。[Google Tasks Help](https://support.google.com/tasks/answer/12132599?hl=en)

这说明 Google 的真实设计是：

- **模型上分离**：Event 和 Task 是不同对象、不同 API。
- **体验上整合**：Task 可以显示在 Calendar 里，用户感觉是“一套时间管理体验”。
- **能力上复用**：周期展开、未来窗口实例化等逻辑可以共享，但对象边界仍然保留。

对 VoiceLife 的启发：

- 即使 UI/语音入口统一，底层对象边界依然可以明确分开。
- “统一体验”不等于“统一模块”，Google 更像是前台整合、后台分层。
- 对 VoiceLife 而言，可以借鉴这种方式：用户只感知一条链路，但工程上保留日程与调度两个模块。

### 3.3 Outlook / Microsoft Graph：Event 与 To Do 分离，周期主从实例模型更显式

Microsoft Graph 中同样是两套对象：

- `event`
- `todoTask`

#### 3.3.1 Event 模型

Graph `event` 资源有几个很强的信号：

- `type` 直接区分 `singleInstance`、`occurrence`、`exception`、`seriesMaster`。[event resource type](https://learn.microsoft.com/en-us/graph/api/resources/event?view=graph-rest-1.0)
- `recurrence` 使用 `patternedRecurrence`，把 `pattern` 和 `range` 明确拆开。[patternedRecurrence](https://learn.microsoft.com/en-us/graph/api/resources/patternedrecurrence?view=graph-rest-1.0)
- `seriesMasterId`、`exceptionOccurrences`、`cancelledOccurrences` 明确表达“主系列 + 例外 + 取消实例”的结构。[event resource type](https://learn.microsoft.com/en-us/graph/api/resources/event?view=graph-rest-1.0)
- `isReminderOn` 与 `reminderMinutesBeforeStart` 直接挂在 event 上。[event resource type](https://learn.microsoft.com/en-us/graph/api/resources/event?view=graph-rest-1.0)
- 若某个事件是 `seriesMaster`，可以通过 `instances` API 获取某时间范围内的 occurrence 与 exception。[List instances](https://learn.microsoft.com/en-us/graph/api/event-list-instances?view=graph-rest-1.0)

#### 3.3.2 To Do / Task 模型

Graph `todoTask` 资源也有：

- `dueDateTime`
- `completedDateTime`
- `isReminderOn`
- `reminderDateTime`
- `recurrence`

也就是说，在 Outlook / Microsoft To Do 体系里：

- 任务不是 event 的降级版。
- 任务本身也有 reminder 和 recurrence。
- 但它依然不和 calendar event 混成同一个资源。[todoTask resource type](https://learn.microsoft.com/en-us/graph/api/resources/todotask?view=graph-rest-1.0)

对 VoiceLife 的启发：

- 如果未来产品要支持“时间点提醒”和“待办完成态”，事件和任务可以共用很多调度能力，但不应失去语义区分。
- Outlook / To Do 体系说明，成熟系统完全可以在对象边界清晰分开的前提下共享周期和提醒能力。
- 这对 VoiceLife 的启发不是“必须合并”，而是“可以分开，只要共享规则与契约”。

## 4. 横向对比

| 维度 | iCalendar | Google Calendar / Tasks | Outlook / To Do | 对 VoiceLife 的含义 |
| --- | --- | --- | --- | --- |
| 一等业务对象 | `VEVENT`、`VTODO` | `Event`、`Task` 分离 | `event`、`todoTask` 分离 | 应以“日程/任务项”做一等对象，而不是 `timer_task` |
| 周期表达 | `RRULE` | Event 用 `recurrence`，Task API 字段表达较弱 | `patternedRecurrence` | 周期规则应挂在源对象或其规则子对象上 |
| 单次例外 | `RECURRENCE-ID` | `recurringEventId` + `originalStartTime` | `occurrence` / `exception` / `seriesMaster` | 应保留实例级修改模型 |
| 提醒表达 | `VALARM` | Event 自带 `reminders` | Event / Task 各自带 reminder 字段 | 提醒应作为对象能力，不必单独再拆提醒模块 |
| 实例获取 | 规范定义实例标识，不定义服务 API | `events.instances()` | `event instances` API | 实例应更像派生视图/运行时对象 |
| “本次及以后” | `RECURRENCE-ID` + `RANGE` | 截断原系列并创建新系列 | 主系列 + 例外 + 范围实例 | 应支持 `single/future/all`，但实现是调度内核责任 |
| 是否单列调度对象 | 否 | 否 | 否 | 不必单独列出 `timer_task` 资源，但独立建模仍然合理 |

## 5. 核心判断：是否需要合并

### 5.1 不建议的方案：把两个模块粗暴揉成一个大模块

如果把以下职责都塞到同一个模块里：

- 日程 CRUD
- 周期规则解析
- 下次触发推进
- 实例生成
- 推迟与关闭
- 回执投递与补偿

那么会出现几个明显问题：

- 业务语义和运行时状态机会强耦合。
- 日程 owner 和调度 owner 难以并行协作。
- 后续排障时，数据问题、规则问题、触发问题会混在一起。

这对 VoiceLife 当前多人协作完善文档和模块的阶段尤其不利。

### 5.2 可以接受且更适合当前阶段的方案：领域层与调度层都分开

保留两个模块并不一定意味着设计错误，前提是把边界定清：

- 日程管理模块负责 `schedule` 作为业务真相源。
- 定时任务模块负责 `timer_task`、`instance` 以及提醒执行链路。
- 两者通过 `schedule_id`、统一时间格式、统一 `change_scope` 语义协作。

这个方案的好处是：

- 职责更单一，便于两个人并行推进。
- 调度复杂度可以继续沉淀，而不污染日程 CRUD。
- 后续如果提醒链路迁移到独立服务，改造成本更低。
- `single/future/all` 等高复杂度语义可以由调度模块专门承接。

### 5.3 推荐方案：模块分开，契约收敛
最合适的答案是：

- **模块分开**：保留 `日程管理模块` 与 `定时任务模块` 两个模块。
- **契约收敛**：统一字段命名、时间规范、修改范围语义和事件流。
- **调用关系单向**：由日程管理模块驱动定时任务模块注册、重算、终止。

一句话概括：

- **模块分开，语义对齐，调用单向。**

## 6. 推荐给 VoiceLife 的模块设计

### 6.1 推荐的顶层模块

建议明确保留三个组成部分，但以前两个模块为主体：

- `日程管理模块`
- `定时任务模块`
- `通知与回执组件`

其中：

- `日程管理模块` 负责 create / query / update / delete，以及向定时任务模块发起注册和变更。
- `定时任务模块` 负责周期展开、窗口实例化、next trigger 推进、snooze/dismiss 状态迁移。
- `通知与回执组件` 负责语音、铃声、IM 回执发送与补偿。

### 6.2 推荐的核心对象

#### 日程模块对象

- `Schedule`
- `OperationRecord`

推荐字段方向：

- `Schedule`
  - `schedule_id`
  - `event`
  - `start_time`
  - `end_time`
  - `location`
  - `notes`
  - `reminder_id`
  - `status`

#### 定时任务模块对象

- `TimerTask`
- `TimerInstance`
- `ReminderConfig`
- `IMOutbox`

这些对象可以继续承接你当前文档里的 `timer_task`、`instance`、`IMOutbox` 能力，并保持为独立调度模型。

### 6.3 推荐的接口边界

建议保留两套接口，但把职责切清：

- 日程管理模块：
  - `create_schedule`
  - `query_schedule`
  - `update_schedule`
  - `delete_schedule`
  - `undo_last_operation`
- 定时任务模块：
  - `RegisterTimerTask`
  - `UpdateTimerTask`
  - `CancelTimerTask`
  - `GenerateInstances`
  - `ListInstances`
  - `SnoozeInstance`
  - `DismissInstance`

推荐调用链：

- 创建日程成功后，调用 `RegisterTimerTask`
- 修改日程时，根据变化范围调用 `UpdateTimerTask`
- 删除日程时，同步调用 `CancelTimerTask`

这样虽然是两个模块，但链路是稳定且可审计的。

### 6.4 修改语义建议

保留你当前文档里已经很成熟的三种范围语义：

- `single`
- `future`
- `all`

但建议把它们定义为**跨模块共享语义**，由两个模块共同遵守。

映射关系可以参考三家做法：

- `single`：改单个 occurrence，形成 exception
- `future`：从目标 occurrence 起切分系列，重建后半段
- `all`：更新整个源系列

其中 `future` 的实现不应是“直接改所有后续实例”，而应更接近：

- 调整原规则边界
- 新建一个新的后续规则段

这与 Google Calendar 的“截断老系列 + 新建新系列”最一致，也最利于后续审计与回溯。[Recurring events](https://developers.google.com/workspace/calendar/api/guides/recurringevents)

## 7. 对现有文档的具体修订建议

### 7.1 对《日程管理模块 - 接口文档》的建议

建议继续保持“纯数据管理”定位，但补充与定时任务模块的协作约定：

- 明确 `schedule` 是业务真相源。
- 明确哪些字段变化需要触发 `RegisterTimerTask` / `UpdateTimerTask` / `CancelTimerTask`。
- 在接口文档中增加与调度模块的联动说明，而不是把调度字段直接塞进日程对象。

### 7.2 对《定时任务模块需求与设计文档 V1》的建议

建议继续保留为独立模块文档，定位调整为：

- 是与日程管理模块配套的调度模块
- 负责承接周期、实例与提醒执行链路

原文档中的这些能力建议保留：

- `next_trigger_at`
- 实例级幂等
- `single/future/all`
- `effective_from`
- `snooze` / `dismiss`
- 历史终态实例保留

建议继续保持现有表达：

- `schedule -> timer_task -> instance`

这样更有利于你们当前协作分工，也便于把“业务数据”“调度计划”“执行实例”分层讨论。

## 8. 最终结论

最终建议如下：

- **不推荐合并“日程管理模块”和“定时任务模块”。**
- **建议继续保留两个独立模块，并通过明确契约协作。**
- **建议把复杂的周期、实例、提醒执行责任稳定放在定时任务模块，而不是回收到日程模块。**

如果必须用一句话回答“是否需要合并”：

- **不需要合并，二者分开更合适。**

## 9. 建议的落地版本

对 VoiceLife 更稳妥的 V2 架构建议是：

1. 保留 `Schedule` 与 `TimerTask` 两套模块边界。
2. `Schedule` 继续作为业务真相源，`TimerTask` 继续作为调度真相源。
3. 通过 `schedule_id`、统一时间格式、统一 `change_scope` 语义保持一致性。
4. `TimerInstance` 继续作为调度模块的实例对象，不回收到日程模块。
5. 回执、提醒发送、失败重试继续与调度层解耦。

这个方案同时兼容：

- 你当前文档已经抽出来的调度复杂度
- Google / Outlook “对象分层、能力共享”的思路
- iCalendar 的标准对象模型

## 10. 参考资料

- [RFC 5545: Internet Calendaring and Scheduling Core Object Specification (iCalendar)](https://www.rfc-editor.org/rfc/rfc5545)
- [Google Calendar API: Events resource](https://developers.google.com/workspace/calendar/api/v3/reference/events)
- [Google Calendar API: Recurring events](https://developers.google.com/workspace/calendar/api/guides/recurringevents)
- [Google Tasks API: REST Resource tasks](https://developers.google.com/workspace/tasks/reference/rest/v1/tasks)
- [Google Tasks Help: Manage repeating tasks in Google Tasks and Google Calendar](https://support.google.com/tasks/answer/12132599?hl=en)
- [Microsoft Graph: event resource type](https://learn.microsoft.com/en-us/graph/api/resources/event?view=graph-rest-1.0)
- [Microsoft Graph: todoTask resource type](https://learn.microsoft.com/en-us/graph/api/resources/todotask?view=graph-rest-1.0)
- [Microsoft Graph: patternedRecurrence resource type](https://learn.microsoft.com/en-us/graph/api/resources/patternedrecurrence?view=graph-rest-1.0)
- [Microsoft Graph: List instances](https://learn.microsoft.com/en-us/graph/api/event-list-instances?view=graph-rest-1.0)
