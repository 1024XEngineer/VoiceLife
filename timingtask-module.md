# 定时任务

## 1. 范围说明

- 本文档只描述“日程创建之后”的定时、周期、触发、推迟、关闭、回执逻辑。
- `CreateSchedule` 属于日程模块，不属于本模块职责。
- 本模块接收日程模块输出的日程数据，并把它转换为可执行的定时任务与实例。
- 字段命名统一采用 `snake_case`。
- 外键统一使用 `<entity>_id`。
- 布尔字段统一使用 `is_` 前缀。
- 时间字段统一使用 `_at` 后缀，时间值采用 ISO 8601。
- 状态字段统一使用 snake_case 字符串，并按实体状态机定义，不描述动作。

## 2. 模块接口

### 2.0 范围参数约定

- `change_scope=single`：只影响某一次具体实例，必须同时提供 `instance_id` 或 `target_occurrence_at`。
- `change_scope=future`：影响“从某次开始及以后”的所有实例，必须同时提供 `effective_from`。
- `change_scope=series`：影响整个周期任务，不需要额外指定单次边界。
- `target_occurrence_at`：目标实例原计划触发时间，用于唯一定位周期中的某一次。
- `effective_from`：从哪个时间点开始向后生效，通常用于“本次及以后”。
- `timer_task.status`：`active` / `paused` / `terminated`。
- `timer_instance.status`：`pending` / `triggered` / `snoozed` / `dismissed` / `skipped` / `modified`。

### 2.1 任务管理

- `RegisterTimerTask`：注册定时任务。
  入参：
  - `schedule_id`：日程 ID。
  - `start_at`：首次触发时间。
  - `recurrence_rule`：周期规则；一次性日程可为空。
  - `reminder_config`：提醒配置。
  出参：
  - `task_id`：生成的定时任务 ID。
  - `status`：注册结果状态，通常为 `active`。
  - `next_trigger_at`：下一次预计触发时间。

- `UpdateTimerTask`：更新定时任务。
  入参：
  - `task_id`：定时任务 ID。
  - `schedule_id`：关联日程 ID。
  - `start_at`：更新后的开始时间。
  - `recurrence_rule`：更新后的周期规则。
  - `reminder_config`：更新后的提醒配置。
  - `change_scope`：修改范围，`single` / `series` / `future`。
  - `instance_id`：当 `change_scope=single` 时可直接指定目标实例 ID。
  - `target_occurrence_at`：当 `change_scope=single` 时，用原计划触发时间定位要修改的那一次。
  - `effective_from`：当 `change_scope=future` 时，表示从哪一次开始向后生效。
  出参：
  - `task_id`：被更新的任务 ID。
  - `status`：更新后的任务状态；若 `change_scope=single`，目标实例状态通常为 `modified`。
  - `next_trigger_at`：重算后的下一次触发时间。

- `CancelTimerTask`：取消定时任务。
  入参：
  - `task_id`：定时任务 ID。
  - `schedule_id`：关联日程 ID。
  - `change_scope`：取消范围，`single` / `series` / `future`。
  - `instance_id`：当 `change_scope=single` 时可直接指定目标实例 ID。
  - `target_occurrence_at`：当 `change_scope=single` 时，用原计划触发时间定位要取消的那一次。
  - `effective_from`：当 `change_scope=future` 时，表示从哪一次开始向后取消。
  出参：
  - `task_id`：被取消的任务 ID。
  - `status`：通常为 `terminated`；若 `change_scope=single`，目标实例状态通常为 `skipped`。

- `PauseTimerTask`：暂停定时任务。
  入参：
  - `task_id`：定时任务 ID。
  - `reason`：暂停原因，可选。
  出参：
  - `task_id`：被暂停的任务 ID。
  - `status`：通常为 `paused`。
  - `paused_until`：计划恢复时间，可选。

- `ResumeTimerTask`：恢复定时任务。
  入参：
  - `task_id`：定时任务 ID。
  - `resume_at`：恢复后从何时开始重新计算下一次触发，可选。
  出参：
  - `task_id`：被恢复的任务 ID。
  - `status`：通常为 `active`。
  - `paused_until`：恢复后置空。
  - `next_trigger_at`：恢复后重新计算出的下一次触发时间。

### 2.2 周期实例

- `GenerateInstances`：生成周期实例。
  入参：
  - `task_id`：定时任务 ID。
  - `window_start`：生成窗口开始时间。
  - `window_end`：生成窗口结束时间。
  - `limit`：最多生成多少个实例，避免无限展开。
  出参：
  - `task_id`：所属任务 ID。
  - `instances`：生成出的实例列表。

- `ListInstances`：查询实例列表。
  入参：
  - `task_id`：定时任务 ID，可选。
  - `schedule_id`：日程 ID，可选。
  - `range_start`：查询开始时间。
  - `range_end`：查询结束时间。
  - `status`：实例状态过滤条件，可选。
  出参：
  - `instances`：符合条件的实例列表。
  - `total`：实例总数。

- `SnoozeInstance`：推迟某个实例。
  入参：
  - `instance_id`：实例 ID。
  - `delay_minutes`：推迟时长，单位分钟。
  出参：
  - `instance_id`：被推迟的实例 ID。
  - `status`：通常为 `snoozed`。
  - `trigger_at`：推迟后的实际触发时间。
  - `delay_count`：推迟后的累计次数。

- `DismissInstance`：关闭某个实例。
  入参：
  - `instance_id`：实例 ID。
  出参：
  - `instance_id`：被关闭的实例 ID。
  - `status`：通常为 `dismissed`。

### 2.3 回执

- `SendReceipt`：发送 IM 回执。
  入参：
  - `instance_id`：实例 ID。
  - `event_type`：事件类型，`triggered` / `snoozed` / `dismissed`。
  - `receipt_channel`：发送通道。
  - `payload`：要发送的具体内容。
  出参：
  - `receipt_id`：回执记录 ID。
  - `status`：发送状态。
  - `retry_count`：当前重试次数。

## 3. 数据模型

### 3.0 `schedule`、`timer_task`、`timer_instance` 的关系

- `schedule`：日程模块中的业务记录，表示“用户想在什么时间做什么事”。
- `timer_task`：定时任务模块中的调度记录，表示“系统根据这条日程，应该如何安排后续触发”。
- `timer_instance`：某一次具体触发实例，表示“这一次提醒本身”。
- 上游 `schedule` 的状态遵循 `active` / `completed` / `cancelled`，本模块只读取不维护。

关系说明：

- 一条 `schedule` 可以对应一条 `timer_task`。
- 一条 `timer_task` 可以派生出一条或多条 `timer_instance`。
- 一次性日程通常对应 1 个实例。
- 周期日程通常会不断生成后续实例，但一般只维护最近一次或一个较小时间窗口内的实例。
- 当用户修改“单次”时，通常是对某个 `timer_instance` 做例外处理，不直接改变整条 `timer_task` 的周期规则。
- 当用户修改“本次及以后”时，需要以 `effective_from` 为边界，重算该时间点之后的任务和实例。

### 3.1 `timer_task`

- `id`：定时任务唯一标识。
- `schedule_id`：关联的日程 ID，表示这条定时任务由哪条日程生成。
- `status`：任务状态，`active` 表示运行中，`paused` 表示暂停，`terminated` 表示永久终止。
- `next_trigger_at`：下一次预计触发时间。
- `paused_until`：暂停恢复时间，仅当 `status=paused` 时有效。
- `created_at`：定时任务创建时间。
- `updated_at`：定时任务最后一次更新时间。
- `deleted_at`：软删除时间，NULL 表示未删除。

### 3.2 `recurrence_rule`

- `frequency`：周期频率，支持 `day` / `week` / `month` / `year`。
- `interval`：周期间隔，例如每 2 天、每 3 周。
- `start_at`：周期锚点时间，用于计算重复规则的起点。
- `timezone`：时区，当前一期统一使用 `+08:00`。
- `by_weekdays`：按周重复时，指定星期几触发，例如周一、周三。
- `by_month_day`：按月重复时，指定每月第几天触发，例如每月 15 日。
- `by_month`：按年重复时，指定在哪几个月触发，例如每年 1 月、7 月。
- `by_work_day`：是否需要支持“工作日”语义，当前待确认。
- `end_type`：周期结束方式，`none` 表示不设结束，`until` 表示到某一时间结束，`count` 表示执行固定次数后结束。
- `end_at`：当 `end_type=until` 时，表示周期结束时间。
- `count`：当 `end_type=count` 时，表示最多执行次数。

### 3.3 `timer_instance`

- `id`：实例唯一标识。
- `task_id`：所属定时任务 ID。
- `schedule_id`：所属日程 ID，便于直接追溯业务来源。
- `planned_at`：按规则计算出的原始计划触发时间。
- `trigger_at`：实际用于触发提醒的时间；如果发生推迟，通常会晚于 `planned_at`。
- `status`：实例状态，`pending` 表示待触发，`triggered` 表示已触发，`snoozed` 表示已推迟，`dismissed` 表示已关闭，`skipped` 表示本次跳过，`modified` 表示本次被修改。
- `delay_count`：当前实例已被推迟的次数。
- `override_fields`：本次实例相对原规则的覆盖字段，仅当 `status=modified` 时使用。
- `last_action_at`：最后一次用户操作或系统状态变更时间。
- `created_at`：实例创建时间。
- `updated_at`：实例最后一次更新时间。
- `deleted_at`：软删除时间，NULL 表示未删除。

### 3.4 `reminder_config`

- `mode`：提醒方式，当前固定为 `voice_plus_ring`，即语音 + 铃声。
- `voice_template_id`：语音播报模板 ID。
- `ringtone_id`：铃声资源 ID。
- `is_snooze_allowed`：是否允许用户执行“推迟”操作。
- `is_dismiss_allowed`：是否允许用户执行“关闭”操作。
- `snooze_options`：可选的推迟时长集合，例如 5 分钟、10 分钟、30 分钟。
- `receipt_channel`：回执发送通道，先预留，后续再确定是否接微信等 IM。
- `created_at`：配置创建时间。
- `updated_at`：配置最后一次更新时间。
- `deleted_at`：软删除时间，NULL 表示未删除。

### 3.5 `receipt_outbox`

- `id`：回执消息记录唯一标识。
- `instance_id`：关联的实例 ID，表示这条回执是由哪次提醒产生的。
- `event_type`：业务事件类型，`triggered` 表示已触发，`snoozed` 表示已推迟，`dismissed` 表示已关闭。
- `payload`：发送给 IM 通道的具体消息内容。
- `status`：发送状态，例如 `pending`、`sent`、`failed`。
- `retry_count`：当前已重试次数。
- `next_retry_at`：下次重试发送时间。
- `created_at`：记录创建时间。
- `updated_at`：记录最后一次更新时间。
- `deleted_at`：软删除时间，NULL 表示未删除。

## 4. 待确认项

- 定时任务最终是否完全落在开发板本地。
- `PauseTimerTask` / `ResumeTimerTask` 的精确定义，是暂停调度还是仅静音提醒。
- 工作日语义是否需要单独字段 `by_work_day`。
