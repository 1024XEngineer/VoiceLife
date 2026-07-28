# MCP Tool 定义

> 当前 Tool 返回的数据均为 JSON 数据，后续可返回文本+JSON 的形式

## 日程 Tools

### create_schedule

创建一条日程。如果时间冲突且未忽略冲突，则仅返回冲突列表而不创建。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| event | string | 是 | 事件标题 |
| start_time | datetime \| null | 否 | 开始时间 |
| end_time | datetime \| null | 否 | 结束时间/开始时间+持续时间=结束时间 |
| location | string \| null | 否 | 地点 |
| notes | string \| null | 否 | 备注 |
| ignore_conflict | boolean | 否 | 是否忽略与其他日程的时间冲突，默认 false |

**返回：**

返回结构化 JSON 数据。

| 字段 | 类型 | 说明 |
|---|---|---|
| created | boolean | 是否成功创建日程 |
| schedule | Schedule \| null | 创建成功后的完整日程；未创建时为 null |
| conflicts | Schedule[] | 与新日程冲突的日程；无冲突时为空数组 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

创建成功：

```json
{
  "created": true,
  "schedule": {
    "schedule_id": 12,
    "event": "项目周会",
    "start_time": "2026-07-29T10:00:00+08:00",
    "end_time": "2026-07-29T11:00:00+08:00",
    "location": "301会议室",
    "notes": null
  },
  "conflicts": [],
  "error": null
}
```

存在时间冲突且未忽略冲突时，不创建日程：

```json
{
  "created": false,
  "schedule": null,
  "conflicts": [
    {
      "schedule_id": 8,
      "event": "看牙医",
      "start_time": "2026-07-29T10:00:00+08:00",
      "end_time": "2026-07-29T10:30:00+08:00"
    }
  ],
  "error": null
}
```

调用失败：

```json
{
  "created": false,
  "schedule": null,
  "conflicts": [],
  "error": {
    "code": "INVALID_TIME",
    "message": "结束时间不能早于开始时间"
  }
}
```

### query_schedule

根据日程 ID、关键词或时间范围查询日程。多个查询条件之间为 AND 关系，结果按开始时间升序排列。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number \| null | 否 | 日程 ID；提供后按 ID 精确查询 |
| keyword | string \| null | 否 | 事件标题关键词，支持模糊匹配 |
| start_from | datetime \| null | 否 | 开始时间范围的下限 |
| start_to | datetime \| null | 否 | 开始时间范围的上限 |
| status | string \| null | 否 | 状态筛选，默认 active；all 表示全部状态 |
| limit | number \| null | 否 | 返回条数，默认 10，最大 50 |
| offset | number \| null | 否 | 分页偏移量，默认 0 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| schedules | Schedule[] | 符合条件的日程列表 |
| total | number | 符合条件的日程总数，不受 limit 和 offset 影响 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "schedules": [
    {
      "schedule_id": 12,
      "event": "项目周会",
      "start_time": "2026-07-29T10:00:00+08:00",
      "end_time": "2026-07-29T11:00:00+08:00",
      "location": "301会议室",
      "notes": null,
      "reminder_id": 5,
      "status": "active"
    }
  ],
  "total": 1,
  "error": null
}
```

### update_schedule

修改一条已有日程。调用前应先查询并确定目标日程的 `schedule_id`。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number | 是 | 要修改的日程 ID |
| event | string \| null | 否 | 新的事件标题 |
| start_time | datetime \| null | 否 | 新的开始时间 |
| end_time | datetime \| null | 否 | 新的结束时间 |
| location | string \| null | 否 | 新的地点 |
| notes | string \| null | 否 | 新的备注 |
| reminder_id | number \| null | 否 | 关联的提醒 ID |
| ignore_conflict | boolean | 否 | 是否忽略时间冲突，默认 false |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| updated | boolean | 是否成功修改日程 |
| schedule | Schedule \| null | 修改后的完整日程 |
| conflicts | Schedule[] | 修改后发生冲突的日程；无冲突时为空数组 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "updated": true,
  "schedule": {
    "schedule_id": 12,
    "event": "项目周会",
    "start_time": "2026-07-29T15:00:00+08:00",
    "end_time": "2026-07-29T16:00:00+08:00",
    "location": "301会议室",
    "notes": null,
    "reminder_id": 5,
    "status": "active"
  },
  "conflicts": [],
  "error": null
}
```

### delete_schedule

删除一条日程。调用前应先查询并确定目标日程的 `schedule_id`。该 Tool 不会自动删除关联提醒。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number | 是 | 要删除的日程 ID |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| schedule_id | number | 被删除的日程 ID |
| deleted | boolean | 是否成功删除 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "schedule_id": 12,
  "deleted": true,
  "error": null
}
```

### query_recent_operations

查询当前用户最近 15 分钟内可撤销的日程操作。如需撤销应先调用该 Tool，找到用户想撤销的操作记录。

**参数：**

无。

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| operations | Operation[] | 可撤销的操作记录，按操作时间倒序排列 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "operations": [
    {
      "operation_id": 102,
      "type": "update",
      "schedule_id": 12,
      "schedule_event": "项目周会",
      "operated_at": "2026-07-29T09:55:00+08:00"
    },
    {
      "operation_id": 101,
      "type": "create",
      "schedule_id": 12,
      "schedule_event": "项目周会",
      "operated_at": "2026-07-29T09:50:00+08:00"
    }
  ],
  "error": null
}
```

### undo_operation

撤销当前用户最近 15 分钟内指定的一条日程创建、修改或删除操作。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| operation_id | number | 是 | 要撤销的操作记录 ID，由 `query_recent_operations` Tool获取 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| undone | boolean | 是否成功撤销 |
| operation | Operation \| null | 被撤销的操作信息 |
| schedule | Schedule \| null | 撤销完成后的日程；撤销创建操作时为 null |
| error | Error \| null | 操作不存在、已过期或调用失败时的错误信息 |

```json
{
  "undone": true,
  "operation": {
    "operation_id": 102,
    "type": "update",
    "schedule_id": 12,
    "schedule_event": "项目周会",
    "operated_at": "2026-07-29T09:55:00+08:00"
  },
  "schedule": {
    "schedule_id": 12,
    "event": "项目周会",
    "start_time": "2026-07-29T10:00:00+08:00",
    "end_time": "2026-07-29T11:00:00+08:00",
    "location": "301会议室",
    "notes": null,
    "reminder_id": 5,
    "status": "active"
  },
  "error": null
}
```

## 提醒 Tools

### add_reminder

创建一条提醒。为日程创建提醒后，应将返回的 `reminder_id` 回写到对应日程。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| target_description | string | 是 | 提醒目标描述，用于生成播报内容 |
| target_time | datetime | 是 | 目标事件发生时间 |
| advance_minutes | number \| null | 是 | 弱提醒提前分钟数；null 表示不启用弱提醒 |
| strong_enabled | boolean | 是 | 是否启用到点后可重试的强提醒 |
| max_retries | number \| null | 否 | 强提醒最大播放次数，默认 3 |
| retry_interval_minutes | number \| null | 否 | 强提醒重试间隔，默认 10 分钟 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| created | boolean | 是否成功创建提醒 |
| reminder | Reminder \| null | 创建后的完整提醒 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "created": true,
  "reminder": {
    "reminder_id": 5,
    "target_description": "项目周会",
    "target_time": "2026-07-29T10:00:00+08:00",
    "advance_minutes": 15,
    "strong_enabled": true,
    "max_retries": 3,
    "retry_interval_minutes": 10,
    "status": "pending",
    "strong_state": null
  },
  "error": null
}
```

### update_reminder

修改一条提醒的配置。该 Tool 不用于推迟或关闭正在触发的强提醒。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_id | number | 是 | 要修改的提醒 ID |
| advance_minutes | number \| null | 否 | 新的弱提醒提前分钟数 |
| strong_enabled | boolean \| null | 否 | 是否启用强提醒 |
| max_retries | number \| null | 否 | 新的最大播放次数 |
| retry_interval_minutes | number \| null | 否 | 新的重试间隔 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| updated | boolean | 是否成功修改提醒 |
| reminder | Reminder \| null | 修改后的完整提醒 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "updated": true,
  "reminder": {
    "reminder_id": 5,
    "target_description": "项目周会",
    "target_time": "2026-07-29T10:00:00+08:00",
    "advance_minutes": 20,
    "strong_enabled": true,
    "max_retries": 3,
    "retry_interval_minutes": 10,
    "status": "pending",
    "strong_state": null
  },
  "error": null
}
```

### remove_reminder

删除一条提醒。删除带提醒的日程时，应先删除提醒，再删除日程。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_id | number | 是 | 要删除的提醒 ID |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| reminder_id | number | 被删除的提醒 ID |
| removed | boolean | 是否成功删除 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "reminder_id": 5,
  "removed": true,
  "error": null
}
```

### snooze_reminder

推迟当前正在触发的强提醒。仅适用于强提醒处于 triggered 状态时。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_id | number | 是 | 要推迟的提醒 ID |
| snooze_minutes | number \| null | 否 | 推迟分钟数；未指定时使用提醒的默认重试间隔 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| reminder_id | number | 被推迟的提醒 ID |
| next_trigger_at | datetime \| null | 下次触发时间 |
| retry_count | number | 推迟后的播放次数计数 |
| snoozed | boolean | 是否成功推迟 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "reminder_id": 5,
  "next_trigger_at": "2026-07-29T10:20:00+08:00",
  "retry_count": 2,
  "snoozed": true,
  "error": null
}
```

### dismiss_reminder

关闭当前正在触发的强提醒，关闭后不再继续播放。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_id | number | 是 | 要关闭的提醒 ID |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| reminder_id | number | 被关闭的提醒 ID |
| dismissed | boolean | 是否成功关闭 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "reminder_id": 5,
  "dismissed": true,
  "error": null
}
```

### query_reminder

根据提醒 ID 查询提醒的配置和当前运行状态。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_id | number | 是 | 要查询的提醒 ID |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| reminder | Reminder \| null | 完整提醒，包括强提醒运行状态 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "reminder": {
    "reminder_id": 5,
    "target_description": "项目周会",
    "target_time": "2026-07-29T10:00:00+08:00",
    "advance_minutes": 15,
    "strong_enabled": true,
    "max_retries": 3,
    "retry_interval_minutes": 10,
    "status": "active",
    "strong_state": {
      "retry_count": 1,
      "phase": "triggered",
      "next_trigger_at": null
    }
  },
  "error": null
}
```
