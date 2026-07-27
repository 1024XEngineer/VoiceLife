# 日程管理模块 - 接口文档

## 1. 概述

### 1.1 模块定位

日程管理模块运行在硬件设备本地，负责日程的创建、查询、修改、删除，以及关联的提醒管理。

对外暴露一组函数接口，供上层 LLM Agent 通过 Function Calling 调用。LLM 负责将用户语音意图解析为结构化参数，调用本模块函数，模块执行本地存储操作并返回结构化结果，LLM 再将结果组织为自然语言，通过 TTS 播报给用户。

---

## 2. 数据模型

### 2.1 Schedule（日程）

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `schedule_id` | number | 自动生成 | 自增主键，全局唯一 |
| `event` | string | 是 | 事件标题，如"开会"、"看牙医" |
| `start_time` | string (ISO 8601) | 否 | 开始时间，含时区偏移，如 `"2026-07-28T10:00:00+08:00"` |
| `end_time` | string (ISO 8601) \| null | 否 | 结束时间。为 null 时表示仅时间点、无持续时长 |
| `location` | string \| null | 否 | 地点，如"301会议室" |
| `notes` | string \| null | 否 | 备注信息 |
| `reminders` | Reminder[] | 否 | 关联的提醒列表，可为空数组 |
| `status` | string | 自动 | 日程状态：`"active"` \| `"cancelled"` |
| `created_at` | string (ISO 8601) | 自动 | 创建时间 |
| `updated_at` | string (ISO 8601) | 自动 | 最后修改时间 |

**示例：**

```json
{
  "schedule_id": 1,
  "event": "项目周会",
  "start_time": "2026-07-28T10:00:00+08:00",
  "end_time": "2026-07-28T11:30:00+08:00",
  "location": "301会议室",
  "notes": "带上季度报告",
  "reminders": [
    {
      "reminder_id": 1,
      "advance_minutes": 10,
      "repeat_interval_minutes": 5,
      "snooze_minutes": 10,
      "status": "pending"
    }
  ],
  "status": "active",
  "created_at": "2026-07-27T14:30:00+08:00",
  "updated_at": "2026-07-27T14:30:00+08:00"
}
```

### 2.2 Reminder（提醒）

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | 自动生成 | 自增主键 |
| `schedule_id` | number | ✅ | 所属日程 ID |
| `advance_minutes` | number | 默认 | 提前多少分钟提醒，如 `10` 表示日程开始前 10 分钟，默认 10 |
| `repeat_interval_minutes` | number | 否 | 触发后每隔多少分钟重复提醒。`0` 或不填表示不重复 |
| `snooze_minutes` | number | 否 | 用户推迟时默认推迟多少分钟，默认 `10` |
| `status` | string | 自动 | 提醒状态：`"pending"` \| `"triggered"` \| `"snoozed"` \| `"dismissed"` |

**设计说明：**
- 一条日程可绑定多条提醒（如"10分钟前提醒一次、5分钟前再提醒一次"）。
- `repeat_interval_minutes > 0` 时，提醒触发后若未被处理，会按间隔持续触发。
- 删除日程时，其下所有提醒自动删除。

---

## 3. 接口定义

### 通用约定

**时间格式：** 所有时间字段统一使用 `string` 类型 + ISO 8601 带时区偏移，例如 `"2026-07-28T10:00:00+08:00"`。

**返回值结构：** 所有函数返回一个对象，成功时包含业务数据，失败时包含错误信息：

```json
// 成功
{ "schedule": { ... } }

// 失败
{
  "error": {
    "code": "SCHEDULE_NOT_FOUND",
    "message": "未找到 ID 为 5 的日程"
  }
}
```

---

### 3.1 create_schedule

创建一条新日程。

**触发场景：** "帮我记一下明天上午十点开会"、"明晚七点提醒我去跑步"

#### 入参

```json
{
  "event": "项目周会",
  "start_time": "2026-07-28T10:00:00+08:00",
  "end_time": "2026-07-28T11:30:00+08:00",
  "location": "301会议室",
  "notes": "带上季度报告",
  "reminders": [
    {
      "advance_minutes": 10,
      "repeat_interval_minutes": 5
    },
    {
      "advance_minutes": 5
    }
  ],
  "ignore_conflict": false
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `event` | string | ✅ | 事件标题 |
| `start_time` | string (ISO 8601) | ✅ | 开始时间 |
| `end_time` | string (ISO 8601) \| null | 否 | 结束时间。为 null 时表示仅时间点，无持续时长 |
| `location` | string | 否 | 地点 |
| `notes` | string | 否 | 备注 |
| `reminders` | object[] | 否 | 提醒配置列表，每个元素包含 `advance_minutes`（必填）、`repeat_interval_minutes`（选填，默认 0）、`snooze_minutes`（选填，默认 10） |
| `ignore_conflict` | boolean | 否 | 是否忽略时间冲突。默认 `false` |

#### 出参

```json
{
  "schedule_id": 1,
  "schedule": { "...完整日程对象..." },
  "conflicts": [
    {
      "schedule_id": 3,
      "event": "看牙医",
      "start_time": "2026-07-28T10:00:00+08:00",
      "end_time": "2026-07-28T10:30:00+08:00"
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `schedule_id` | number | 新创建的日程 ID。**仅当 `conflicts` 为空或 `ignore_conflict=true` 时有效** |
| `schedule` | Schedule | 完整的日程对象。冲突未解决时为 null |
| `conflicts` | Schedule[] | 冲突的日程列表。无冲突时为空数组 `[]` |

#### 交互流程

```
用户："明天上午十点开会"
  ↓
LLM 解析 → create_schedule(event="开会", start_time="2026-07-28T10:00:00+08:00")
  ↓
返回 conflicts=[{"看牙医", 10:00-10:30}]
  ↓
LLM → TTS："明天上午十点已经有一条'看牙医'的日程，要覆盖吗？"
  ↓
选项 A：用户说"那算了" → 流程结束
选项 B：用户说"还是创建" → LLM 再次调用 create_schedule(ignore_conflict=true)
```

---

### 3.2 query_schedule

按条件查询日程。

**触发场景：** "我明天有什么安排"、"下周有什么事情"、"查一下关于开会的日程"

#### 入参

```json
{
  "schedule_id": null,
  "keyword": "开会",
  "start_from": "2026-07-28T00:00:00+08:00",
  "start_to": "2026-07-28T23:59:59+08:00",
  "status": "active",
  "limit": 10,
  "offset": 0
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `schedule_id` | number | 否 | 按 ID 精确查询。传入此参数时忽略其他筛选条件 |
| `keyword` | string | 否 | 按事件标题模糊匹配 |
| `start_from` | string (ISO 8601) | 否 | 查询此时间之后的日程 |
| `start_to` | string (ISO 8601) | 否 | 查询此时间之前的日程 |
| `status` | string | 否 | 日程状态筛选，默认 `"active"`。传 `"all"` 返回所有 |
| `limit` | number | 否 | 返回条数上限，默认 `10`，最大 `50` |
| `offset` | number | 否 | 分页偏移，默认 `0` |

**查询规则：**
- 至少传入一个筛选条件（`schedule_id`、`keyword`、`start_from` / `start_to` 中的至少一项），否则返回错误。
- 多个条件为 AND 关系。
- 结果按 `start_time` 升序排列。

#### 出参

```json
{
  "schedules": [
    {
      "schedule_id": 1,
      "event": "项目周会",
      "start_time": "2026-07-28T10:00:00+08:00",
      "end_time": "2026-07-28T11:30:00+08:00",
      "location": "301会议室",
      "notes": "带上季度报告",
      "reminders": [ ],
      "status": "active",
      "created_at": "2026-07-27T14:30:00+08:00",
      "updated_at": "2026-07-27T14:30:00+08:00"
    }
  ],
  "total": 1
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `schedules` | Schedule[] | 匹配的日程列表 |
| `total` | number | 总匹配数（不受 limit/offset 影响） |

---

### 3.3 update_schedule

修改一条已有日程。

**触发场景：** "把明天开会改成下午三点"、"给开会加上地点301会议室"

**调用前提：** 调用方（LLM）必须先通过 `query_schedule` 定位到目标日程的 `schedule_id`。如果用户表述模糊（如只说"那个开会"），LLM 应查询后列出候选项让用户二次确认，确认后再调用本函数。

#### 入参

```json
{
  "schedule_id": 1,
  "event": "项目周会",
  "start_time": "2026-07-28T15:00:00+08:00",
  "end_time": "2026-07-28T16:30:00+08:00",
  "location": "302会议室",
  "notes": null,
  "reminders": null,
  "ignore_conflict": false
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `schedule_id` | number | ✅ | 目标日程 ID |
| `event` | string | 否 | 新的事件标题 |
| `start_time` | string (ISO 8601) | 否 | 新的开始时间 |
| `end_time` | string (ISO 8601) \| null | 否 | 新的结束时间。传 null 表示清除结束时间 |
| `location` | string \| null | 否 | 新的地点。传 null 表示清除 |
| `notes` | string \| null | 否 | 新的备注。传 null 表示清除 |
| `reminders` | object[] \| null | 否 | 新的提醒列表。**注意：传入此参数会完全替换现有提醒列表。** 传 null 表示不修改提醒 |
| `ignore_conflict` | boolean | 否 | 是否忽略时间冲突。默认 `false` |

**部分更新规则：** 仅更新传入的非 null 字段，未传入的字段保持原值。

#### 出参

```json
{
  "schedule_id": 1,
  "schedule": { "...更新后的完整日程..." },
  "conflicts": []
}
```

同 `create_schedule`。

#### 交互流程

```
用户："把开会改成下午三点"
  ↓
LLM 先调用 query_schedule(keyword="开会")
  ↓
返回 2 条结果：①上午十点开会、②下午三点项目会议
  ↓
LLM → TTS："找到两个开会相关的日程：第一个，明天上午十点；第二个，明天下午三点项目会议。要改哪一个？"
  ↓
用户："第一个"
  ↓
LLM 调用 update_schedule(schedule_id=5, start_time="15:00")
```

---

### 3.4 delete_schedule

删除一条日程。

**触发场景：** "取消明天的开会"、"删掉看牙医的日程"

**调用前提：** 与 `update_schedule` 相同，LLM 必须先确认目标 `schedule_id`。

#### 入参

```json
{
  "schedule_id": 1
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `schedule_id` | number | ✅ | 要删除的日程 ID |

#### 出参

```json
{
  "schedule_id": 1,
  "deleted": true
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `schedule_id` | number | 被删除的日程 ID |
| `deleted` | boolean | 删除是否成功 |

**副作用：** 删除日程时，其下所有关联的提醒自动级联删除。

---

### 3.5 add_reminder

为已有日程追加一条提醒。

**触发场景：** "开会前十分钟提醒我"（日程已存在，仅追加提醒）

#### 入参

```json
{
  "schedule_id": 1,
  "advance_minutes": 10,
  "repeat_interval_minutes": 5,
  "snooze_minutes": 10
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `schedule_id` | number | ✅ | 目标日程 ID |
| `advance_minutes` | number | ✅ | 提前多少分钟提醒 |
| `repeat_interval_minutes` | number | 否 | 重复提醒间隔，`0` 或不传表示不重复 |
| `snooze_minutes` | number | 否 | 推迟时的默认推迟分钟数，默认 `10` |

#### 出参

```json
{
  "reminder_id": 2,
  "schedule_id": 1
}
```

---

### 3.6 delete_reminder

删除日程中的一条提醒。

**触发场景：** "不用提醒我开会了"

#### 入参

```json
{
  "reminder_id": 2
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 要删除的提醒 ID |

#### 出参

```json
{
  "reminder_id": 2,
  "deleted": true
}
```

---

### 3.7 snooze_reminder

推迟一个当前正在触发的提醒。

**触发场景：** 提醒响铃时用户说"十分钟后再提醒我"

#### 入参

```json
{
  "reminder_id": 1,
  "snooze_minutes": 10
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 要推迟的提醒 ID |
| `snooze_minutes` | number | 否 | 推迟分钟数，默认使用提醒本身配置的 `snooze_minutes` |

#### 出参

```json
{
  "reminder_id": 1,
  "next_trigger_at": "2026-07-28T09:55:00+08:00",
  "snoozed": true
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `reminder_id` | number | 提醒 ID |
| `next_trigger_at` | string (ISO 8601) | 下次触发时间 |
| `snoozed` | boolean | 是否推迟成功 |

---

### 3.8 dismiss_reminder

关闭一个当前正在触发的提醒，不再重复。

**触发场景：** 提醒响铃时用户说"知道了"、"关闭提醒"

#### 入参

```json
{
  "reminder_id": 1
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 要关闭的提醒 ID |

#### 出参

```json
{
  "reminder_id": 1,
  "dismissed": true
}
```

---

## 4. 系统事件

提醒触发时，日程模块通过回调/事件机制通知上层，结构如下：

```json
{
  "event_type": "reminder_triggered",
  "reminder_id": 1,
  "schedule_id": 1,
  "schedule_event": "项目周会",
  "schedule_start_time": "2026-07-28T10:00:00+08:00",
  "triggered_at": "2026-07-28T09:50:00+08:00",
  "tts_message": "提醒：你的'项目周会'将在 10 分钟后开始"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `event_type` | string | 事件类型，固定为 `"reminder_triggered"` |
| `reminder_id` | number | 触发的提醒 ID |
| `schedule_id` | number | 关联的日程 ID |
| `schedule_event` | string | 日程标题 |
| `schedule_start_time` | string (ISO 8601) | 日程开始时间 |
| `triggered_at` | string (ISO 8601) | 提醒实际触发时间 |
| `tts_message` | string | 预生成的 TTS 播报文本，可直接传给语音模块 |

---

## 5. 错误码

| 错误码 | 说明 | 触发场景 |
|--------|------|----------|
| `SCHEDULE_NOT_FOUND` | 日程不存在 | update / delete 时传入无效的 schedule_id |
| `REMINDER_NOT_FOUND` | 提醒不存在 | delete_reminder / snooze / dismiss 时传入无效的 reminder_id |
| `INVALID_TIME` | 时间不合法 | start_time 格式错误、end_time 早于 start_time |
| `INVALID_REMINDER` | 提醒配置不合法 | advance_minutes 为负数、reminder 数量超限 |
| `NO_QUERY_CONDITION` | 未提供查询条件 | query_schedule 所有筛选参数均为 null |
| `ALREADY_CANCELLED` | 日程已取消 | 对已取消的日程执行 update |
| `REMINDER_ALREADY_DISMISSED` | 提醒已关闭 | 对已 dismissed 的提醒执行 snooze |
| `INTERNAL_ERROR` | 内部错误 | 存储读写异常等 |

---

## 6. 完整交互时序示例

### 场景 A：创建日程 + 冲突处理

```
用户：     "明天上午十点我要开会"
LLM：      create_schedule(event="开会", start_time="2026-07-28T10:00:00+08:00")
模块返回： { conflicts: [{ event: "看牙医", start_time: "...10:00..." }] }
LLM→TTS：  "明天上午十点已经有一条'看牙医'的日程，还要创建吗？"
用户：     "还是创建吧"
LLM：      create_schedule(event="开会", start_time="...", ignore_conflict=true)
模块返回： { schedule_id: 5, schedule: {...}, conflicts: [] }
LLM→TTS：  "好的，已添加明天上午十点开会"
```

### 场景 B：模糊查询 + 二次确认 + 修改

```
用户：     "把那个开会改到下午三点"
LLM：      query_schedule(keyword="开会")
模块返回： { schedules: [
             { id: 1, event: "开会", start_time: "...明天10:00" },
             { id: 3, event: "项目会议", start_time: "...明天14:00" }
           ], total: 2 }
LLM→TTS：  "找到两个相关的日程：第一个，明天上午十点开会；第二个，明天下午两点项目会议。你要改哪一个？"
用户：     "第一个"
LLM：      update_schedule(schedule_id=1, start_time="2026-07-28T15:00:00+08:00")
模块返回： { schedule_id: 1, schedule: {...}, conflicts: [] }
LLM→TTS：  "已把开会改为明天下午三点"
```

### 场景 C：提醒触发 + 推迟

```
系统事件： reminder_triggered → TTS 播报"提醒：你的'项目周会'将在 10 分钟后开始"
用户：     "十分钟后再提醒我"
LLM：      snooze_reminder(reminder_id=1, snooze_minutes=10)
模块返回： { snoozed: true, next_trigger_at: "...09:55:00" }
LLM→TTS：  "好的，十分钟后再提醒你"
```

---

## 7. Mock 数据参考

前期开发可直接使用以下 mock 返回值对接，无需等待存储层实现。

### mock: create_schedule 成功

```json
{
  "schedule_id": 1,
  "schedule": {
    "schedule_id": 1,
    "event": "项目周会",
    "start_time": "2026-07-28T10:00:00+08:00",
    "end_time": "2026-07-28T11:30:00+08:00",
    "location": "301会议室",
    "notes": null,
    "reminders": [],
    "status": "active",
    "created_at": "2026-07-27T14:30:00+08:00",
    "updated_at": "2026-07-27T14:30:00+08:00"
  },
  "conflicts": []
}
```

### mock: create_schedule 冲突

```json
{
  "schedule_id": null,
  "schedule": null,
  "conflicts": [
    {
      "schedule_id": 2,
      "event": "看牙医",
      "start_time": "2026-07-28T10:00:00+08:00",
      "end_time": "2026-07-28T10:30:00+08:00",
      "status": "active"
    }
  ]
}
```

### mock: query_schedule 今天所有日程

```json
{
  "schedules": [
    {
      "schedule_id": 1,
      "event": "项目周会",
      "start_time": "2026-07-28T10:00:00+08:00",
      "end_time": "2026-07-28T11:30:00+08:00",
      "location": "301会议室",
      "notes": null,
      "reminders": [
        {
          "reminder_id": 1,
          "advance_minutes": 10,
          "repeat_interval_minutes": 0,
          "snooze_minutes": 10,
          "status": "pending"
        }
      ],
      "status": "active",
      "created_at": "2026-07-27T14:30:00+08:00",
      "updated_at": "2026-07-27T14:30:00+08:00"
    },
    {
      "schedule_id": 2,
      "event": "看牙医",
      "start_time": "2026-07-28T08:00:00+08:00",
      "end_time": "2026-07-28T08:30:00+08:00",
      "location": "市口腔医院",
      "notes": null,
      "reminders": [],
      "status": "active",
      "created_at": "2026-07-26T20:00:00+08:00",
      "updated_at": "2026-07-26T20:00:00+08:00"
    }
  ],
  "total": 2
}
```

