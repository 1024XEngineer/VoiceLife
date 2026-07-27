# 定时任务

## 1. 范围说明

- 本文档只描述“日程创建之后”的定时、周期、触发、推迟、关闭、回执逻辑。
- `CreateSchedule` 属于日程模块，不属于本模块职责。
- 本模块接收日程模块输出的日程数据，并把它转换为可执行的定时任务与实例。


## 2. 模块接口

### 2.1 任务管理

- `RegisterTimerTask`：注册定时任务
- `UpdateTimerTask`：更新定时任务
- `CancelTimerTask`：取消定时任务

```
- `PauseTimerTask`：暂停定时任务
- `ResumeTimerTask`：恢复定时任务
是否考虑？
```

### 2.2 周期实例

- `GenerateInstances`：生成周期实例
- `ListInstances`：查询实例列表
- `SnoozeInstance`：推迟某个实例
- `DismissInstance`：关闭某个实例

### 2.3 回执

- `SendReceipt`：发送 IM 回执

## 3. 数据模型

### 3.1 `TimerTask`

- `id`
- `scheduleId`
- `status`：`active` / `paused` / `canceled`
- `nextTriggerAt`
- `createdAt`
- `updatedAt`

### 3.2 `RecurrenceRule`

- `frequency`：`day` / `week` / `month` / `year`
- `interval`
- `byWeekdays`
- `byMonthDay`
- `byMonth`
```
- `byWorkDay` 
是否考虑?
```
- `endType`：`none` / `until` / `count`
- `endAt`
- `count`

### 3.3 `TimerInstance`

- `id`
- `taskId`
- `scheduleId`
- `plannedAt`
- `triggerAt`
- `status`：`pending` / `triggered` / `snoozed` / `dismissed` / `expired` / `canceled`
- `snoozeCount`
- `lastActionAt`

### 3.4 `ReminderConfig`

- `mode`：`voice+ring`
- `voiceTemplateId`
- `ringtoneId`
- `allowSnooze`
- `allowDismiss`
- `snoozeOptions`
- `receiptChannel`

### 3.5 `IMOutbox`

- `id`
- `instanceId`
- `eventType`：`triggered` / `snoozed` / `dismissed`
- `payload`
- `sendStatus`
- `retryCount`
- `nextRetryAt`


## 4. 待确认项

- 定时任务最终是否完全落在开发板本地
- 工作日是否包含