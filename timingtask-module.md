# 定时任务

## 1. 范围说明

- 本文档只描述“日程创建之后”的定时、周期、触发、推迟、关闭、回执逻辑。
- `CreateSchedule` 属于日程模块，不属于本模块职责。
- 本模块接收日程模块输出的日程数据，并把它转换为可执行的定时任务与实例。


## 2. 模块接口

### 2.1 任务管理

- `RegisterTimerTask`：注册定时任务
  入参：
  - `scheduleId`：日程 ID。
  - `startAt`：首次触发时间。
  - `recurrenceRule`：周期规则；一次性日程可为空。
  - `reminderConfig`：提醒配置。
  出参：
  - `taskId`：生成的定时任务 ID。
  - `status`：注册结果状态，通常为 `active`。
  - `nextTriggerAt`：下一次预计触发时间。

- `UpdateTimerTask`：更新定时任务
  入参：
  - `taskId`：定时任务 ID。
  - `scheduleId`：关联日程 ID。
  - `startAt`：更新后的开始时间。
  - `recurrenceRule`：更新后的周期规则。
  - `reminderConfig`：更新后的提醒配置。
  - `changeScope`：修改范围，`single` / `series` / `future`。
  出参：
  - `taskId`：被更新的任务 ID。
  - `status`：更新后的任务状态。
  - `nextTriggerAt`：重算后的下一次触发时间。

- `CancelTimerTask`：取消定时任务
  入参：
  - `taskId`：定时任务 ID。
  - `scheduleId`：关联日程 ID。
  - `changeScope`：取消范围，`single` / `series` / `future`。
  出参：
  - `taskId`：被取消的任务 ID。
  - `status`：通常为 `canceled`。

- `PauseTimerTask`：暂停定时任务
  入参：
  - `taskId`：定时任务 ID。
  - `reason`：暂停原因，可选。
  出参：
  - `taskId`：被暂停的任务 ID。
  - `status`：通常为 `paused`。

- `ResumeTimerTask`：恢复定时任务
  入参：
  - `taskId`：定时任务 ID。
  - `resumeAt`：恢复后从何时开始重新计算下一次触发，可选。
  出参：
  - `taskId`：被恢复的任务 ID。
  - `status`：通常为 `active`。
  - `nextTriggerAt`：恢复后重新计算出的下一次触发时间。

### 2.2 周期实例

- `GenerateInstances`：生成周期实例
  入参：
  - `taskId`：定时任务 ID。
  - `windowStart`：生成窗口开始时间。
  - `windowEnd`：生成窗口结束时间。
  - `limit`：最多生成多少个实例，避免无限展开。
  出参：
  - `taskId`：所属任务 ID。
  - `instances`：生成出的实例列表。

- `ListInstances`：查询实例列表
  入参：
  - `taskId`：定时任务 ID，可选。
  - `scheduleId`：日程 ID，可选。
  - `rangeStart`：查询开始时间。
  - `rangeEnd`：查询结束时间。
  - `status`：实例状态过滤条件，可选。
  出参：
  - `instances`：符合条件的实例列表。
  - `total`：实例总数。

- `SnoozeInstance`：推迟某个实例
  入参：
  - `instanceId`：实例 ID。
  - `delayMinutes`：推迟时长，单位分钟。
  出参：
  - `instanceId`：被推迟的实例 ID。
  - `status`：通常为 `snoozed`。
  - `triggerAt`：推迟后的实际触发时间。
  - `snoozeCount`：推迟后的累计次数。

- `DismissInstance`：关闭某个实例
  入参：
  - `instanceId`：实例 ID。
  出参：
  - `instanceId`：被关闭的实例 ID。
  - `status`：通常为 `dismissed`。

### 2.3 回执

- `SendReceipt`：发送 IM 回执
  入参：
  - `instanceId`：实例 ID。
  - `eventType`：事件类型，`triggered` / `snoozed` / `dismissed`。
  - `receiptChannel`：发送通道。
  - `payload`：要发送的具体内容。
  出参：
  - `receiptId`：回执记录 ID。
  - `sendStatus`：发送状态。
  - `retryCount`：当前重试次数。

## 3. 数据模型

### 3.0 `Schedule`、`TimerTask`、`TimerInstance` 的关系

- `Schedule`：日程模块中的业务记录，表示“用户想在什么时间做什么事”。
- `TimerTask`：定时任务模块中的调度记录，表示“系统根据这条日程，应该如何安排后续触发”。
- `TimerInstance`：某一次具体触发实例，表示“这一次提醒本身”。

关系说明：

- 一条 `Schedule` 可以对应一条 `TimerTask`。
- 一条 `TimerTask` 可以派生出一条或多条 `TimerInstance`。
- 一次性日程通常对应 1 个实例。
- 周期日程通常会不断生成后续实例，但一般只维护最近一次或一个较小时间窗口内的实例。

### 3.1 `TimerTask`

- `id`：定时任务唯一标识。
- `scheduleId`：关联的日程 ID，表示这条定时任务由哪条日程生成。
- `status`：任务状态，`active` 表示运行中，`paused` 表示暂停，`canceled` 表示已取消。
- `nextTriggerAt`：下一次预计触发时间。
- `createdAt`：定时任务创建时间。
- `updatedAt`：定时任务最后一次更新时间。

### 3.2 `RecurrenceRule`

- `frequency`：周期频率，支持 `day` / `week` / `month` / `year`。
- `interval`：周期间隔，例如每 2 天、每 3 周。
- `byWeekdays`：按周重复时，指定星期几触发，例如周一、周三。
- `byMonthDay`：按月重复时，指定每月第几天触发，例如每月 15 日。
- `byMonth`：按年重复时，指定在哪几个月触发，例如每年 1 月、7 月。
- `byWorkDay`：是否需要支持“工作日”语义，当前待确认。
- `endType`：周期结束方式，`none` 表示不设结束，`until` 表示到某一时间结束，`count` 表示执行固定次数后结束。
- `endAt`：当 `endType=until` 时，表示周期结束时间。
- `count`：当 `endType=count` 时，表示最多执行次数。

### 3.3 `TimerInstance`

- `id`：实例唯一标识。
- `taskId`：所属定时任务 ID。
- `scheduleId`：所属日程 ID，便于直接追溯业务来源。
- `plannedAt`：按规则计算出的原始计划触发时间。
- `triggerAt`：实际用于触发提醒的时间；如果发生推迟，通常会晚于 `plannedAt`。
- `status`：实例状态，`pending` 表示待触发，`triggered` 表示已触发，`snoozed` 表示已推迟，`dismissed` 表示已关闭，`expired` 表示过期未处理，`canceled` 表示已取消。
- `snoozeCount`：当前实例已被推迟的次数。
- `lastActionAt`：最后一次用户操作或系统状态变更时间。

### 3.4 `ReminderConfig`

- `mode`：提醒方式，当前固定为 `voice+ring`，即语音 + 铃声。
- `voiceTemplateId`：语音播报模板 ID。
- `ringtoneId`：铃声资源 ID。
- `allowSnooze`：是否允许用户执行“推迟”操作。
- `allowDismiss`：是否允许用户执行“关闭”操作。
- `snoozeOptions`：可选的推迟时长集合，例如 5 分钟、10 分钟、30 分钟。
- `receiptChannel`：回执发送通道，先预留，后续再确定是否接微信等 IM。

### 3.5 `IMOutbox`

- `id`：回执消息记录唯一标识。
- `instanceId`：关联的实例 ID，表示这条回执是由哪次提醒产生的。
- `eventType`：业务事件类型，`triggered` 表示已触发，`snoozed` 表示已推迟，`dismissed` 表示已关闭。
- `payload`：发送给 IM 通道的具体消息内容。
- `sendStatus`：发送状态，例如待发送、发送成功、发送失败。
- `retryCount`：当前已重试次数。
- `nextRetryAt`：下次重试发送时间。


## 4. 待确认项

- 定时任务最终是否完全落在开发板本地
- 是否支持 `PauseTimerTask` / `ResumeTimerTask`
- 工作日语义是否需要单独字段 `byWorkDay`
