# 日程管理模块 - 接口文档

## 1. 概述

### 1.1 模块定位

日程管理模块运行在硬件设备本地，负责日程的创建、查询、修改、删除。它是纯粹的数据管理模块，不承载任何提醒、通知、定时调度的执行逻辑。

对外暴露一组函数接口，供上层 LLM Agent 通过 Function Calling 调用。LLM 负责将用户语音意图解析为结构化参数，调用本模块函数，模块执行本地存储操作并返回结构化结果，LLM 再将结果组织为自然语言，通过 TTS 播报给用户。

### 1.2 职责边界

```
用户语音 → ASR → LLM Agent（意图识别 + 参数提取）
                       │
           ┌───────────┼───────────┐
           ▼           ▼           ▼
   ┌────────────┐ ┌──────────┐ ┌──────────┐
   │ 日程管理模块 │ │ 提醒模块  │ │ 其他模块  │
   │            │ │          │ │          │
   │ · CRUD     │ │ · 弱提醒  │ │          │
   │ · 冲突检测  │ │ · 强提醒  │ │          │
   │            │ │ · 重试    │ │          │
   └────────────┘ │ · 推迟    │ └──────────┘
                  └──────────┘
```

**本模块负责：**
- 日程的创建、查询、修改、删除
- 时间冲突检测

**本模块不负责（由其他模块承载）：**
- 语音识别（ASR）
- 自然语言时间解析（由 LLM 在调用前完成）
- 提醒配置与执行（由提醒模块独立负责）
- 重复/定时事件调度（由独立模块处理）
- TTS 语音合成

### 1.3 与提醒模块的关系

日程模块与提醒模块**完全解耦**。日程不携带任何提醒字段，两者是平级的独立服务。

LLM 在解析用户意图时，自行判断需要调用哪个模块：

```
用户："明天上午十点开会，提前 15 分钟提醒我"
  ↓
LLM 判断：
  1. 先调日程模块 create_schedule(event="开会", start_time="...")，拿到 schedule_id=1
  2. 再调提醒模块 add_reminder(target_description="开会",
                                target_time="2026-07-28T10:00:00+08:00",
                                advance_minutes=15)
  3. 提醒模块返回 reminder_id=1
  4. 调日程模块 update_schedule(schedule_id=1, reminder_id=[1])
```

**reminder_id 的维护：** 日程模块只存数字列表，不解析含义。LLM 在创建/删除提醒后主动更新日程的 `reminder_id`，保持两边一致。

日程模块不需要知道提醒模块的存在。提醒模块也不依赖日程模块——它可以独立提醒任何事务（吃药、取快递等），不仅限于日程。

---

## 2. 数据模型

### Schedule（日程）

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `schedule_id` | number | 自动生成 | 自增主键，全局唯一 |
| `event` | string | ✅ | 事件标题，如"开会"、"看牙医" |
| `start_time` | string (ISO 8601) | ✅ | 开始时间，含时区偏移，如 `"2026-07-28T10:00:00+08:00"` |
| `end_time` | string (ISO 8601) \| null | 否 | 结束时间。为 null 时表示仅时间点、无持续时长 |
| `location` | string \| null | 否 | 地点，如"301会议室" |
| `notes` | string \| null | 否 | 备注信息 |
| `reminder_id` | number \| null | 自动 | 关联的提醒 ID。`null` 表示无提醒。LLM 在创建/删除提醒后通过 `update_schedule` 更新此字段 |
| `status` | string | 自动 | 日程状态：`"active"` \| `"cancelled"` |
| `created_at` | string (ISO 8601) | 自动 | 创建时间 |
| `updated_at` | string (ISO 8601) | 自动 | 最后修改时间 |

**示例——有时间段、有地点、有备注：**

```json
{
  "schedule_id": 1,
  "event": "项目周会",
  "start_time": "2026-07-28T10:00:00+08:00",
  "end_time": "2026-07-28T11:30:00+08:00",
  "location": "301会议室",
  "notes": "带上季度报告",
  "reminder_id": 1,
  "status": "active",
  "created_at": "2026-07-27T14:30:00+08:00",
  "updated_at": "2026-07-27T14:30:00+08:00"
}
```

**示例——最简（仅备忘）：**

```json
{
  "schedule_id": 2,
  "event": "车停在 A130",
  "start_time": "2026-07-27T20:00:00+08:00",
  "end_time": null,
  "location": null,
  "notes": null,
  "reminder_id": null,
  "status": "active",
  "created_at": "2026-07-27T18:00:00+08:00",
  "updated_at": "2026-07-27T18:00:00+08:00"
}
```


### OperationRecord（操作记录）

撤销操作返回的操作记录与操作前的完整快照。

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 操作类型：`"create"` \| `"update"` \| `"delete"` |
| `schedule_id` | number | 涉及的日程 ID |
| `schedule_event` | string | 操作时该日程的事件标题。对 `delete` 操作而言是删除前的标题 |
| `operated_at` | string (ISO 8601) | 操作发生时间 |
| `previous` | Schedule \| null | 操作前该日程的完整状态。`create` 操作时为 null（创建前不存在）。`update` 和 `delete` 时为操作前的完整 Schedule 对象 |

> **撤销结果：** 撤销后日程恢复到 `previous` 的状态——
> - 撤销 create：日程被删除
> - 撤销 update：日程的时间、地点、备注、`reminder_id` 等全部字段回退到 `previous` 的值
> - 撤销 delete：日程以 `previous` 的状态重新创建
>
> 操作日志在 15 分钟后自动过期，过期后 `undo_last_operation` 返回 `NO_RECENT_OPERATION`。

**示例（撤销 create）：**

```json
{
  "type": "create",
  "schedule_id": 1,
  "schedule_event": "项目周会",
  "operated_at": "2026-07-27T15:00:00+08:00",
  "previous": null
}
```

**示例（撤销 update — 会议从 10:00 推迟到 11:00 后撤回）：**

```json
{
  "type": "update",
  "schedule_id": 1,
  "schedule_event": "项目周会",
  "operated_at": "2026-07-27T15:05:00+08:00",
  "previous": {
    "schedule_id": 1,
    "event": "项目周会",
    "start_time": "2026-07-28T10:00:00+08:00",
    "end_time": "2026-07-28T11:00:00+08:00",
    "location": "301会议室",
    "notes": null,
    "reminder_id": 1,
    "status": "active",
    "created_at": "2026-07-27T14:30:00+08:00",
    "updated_at": "2026-07-27T14:30:00+08:00"
  }
}
```

---

## 3. 接口定义

### 通用约定

**时间格式：** 所有时间字段统一使用 `string` 类型 + ISO 8601 带时区偏移，例如 `"2026-07-28T10:00:00+08:00"`。

> **为什么不用 datetime 类型？**
> 本接口面向 LLM Function Calling，所有参数和返回值均通过 JSON 序列化传输。JSON 规范不支持原生 `datetime` 类型，因此契约层面必须声明为 `string`。模块内部实现可以使用任意语言的本地 datetime 类型（Python 的 `datetime`、C++ 的 `std::chrono`、JavaScript 的 `Date` 等），在序列化/反序列化时按 ISO 8601 格式相互转换即可。

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

**触发场景：** "帮我记一下明天上午十点开会"、"我车停在 A130"

#### 入参

```json
{
  "event": "项目周会",
  "start_time": "2026-07-28T10:00:00+08:00",
  "end_time": "2026-07-28T11:30:00+08:00",
  "location": "301会议室",
  "notes": "带上季度报告",
  "ignore_conflict": false
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `event` | string | ✅ | 事件标题 |
| `start_time` | string (ISO 8601) | ✅ | 开始时间 |
| `end_time` | string (ISO 8601) \| null | 否 | 结束时间。为 null 时表示仅时间点、无持续时长 |
| `location` | string | 否 | 地点 |
| `notes` | string | 否 | 备注 |
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

**触发场景：** "我明天有什么安排"、"下周有什么事情"、"查一下关于停车的备忘"

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
      "status": "active",
      "created_at": "2026-07-27T14:30:00+08:00",
      "updated_at": "2026-07-27T14:30:00+08:00"
    },
    {
      "schedule_id": 2,
      "event": "车停在 A130",
      "start_time": "2026-07-27T20:00:00+08:00",
      "end_time": null,
      "location": null,
      "notes": null,
      "status": "active",
      "created_at": "2026-07-27T18:00:00+08:00",
      "updated_at": "2026-07-27T18:00:00+08:00"
    }
  ],
  "total": 2
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `schedules` | Schedule[] | 匹配的日程列表 |
| `total` | number | 总匹配数（不受 limit/offset 影响） |

---

### 3.3 update_schedule

修改一条已有日程。

**触发场景：** "把明天开会改成下午三点"、"给开会加上地点301会议室"、"备注改成带上报告"

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
  "reminder_id": null,
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
| `reminder_id` | number \| null | 否 | 新的提醒 ID。传 `null` 表示清除提醒关联。**不传该字段**（参数级别 null）表示不修改 |
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
用户："把那个开会的日程改成下午三点"
  ↓
LLM 先调用 query_schedule(keyword="开会")
  ↓
返回 2 条结果：①上午十点开会、②下午三点项目会议
  ↓
LLM → TTS："找到两个相关的日程：第一个，明天上午十点开会；第二个，明天下午两点项目会议。你要改哪一个？"
  ↓
用户："第一个"
  ↓
LLM 调用 update_schedule(schedule_id=1, start_time="2026-07-28T15:00:00+08:00")
```

---

### 3.4 delete_schedule

删除一条日程。

**触发场景：** "取消明天的开会"、"删掉停车备忘"

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

---

### 3.5 undo_last_operation

撤销最近一次日程操作。

**触发场景：** "撤销刚才的操作"、"撤回"

#### 入参

无参数。接口自动定位当前用户最近15 分钟日程操作记录（最多 3 条数据）。

#### 出参

**成功（有可撤销的操作）：**

```json
{
  "undone": true,
  "operation": {
    "type": "create",
    "schedule_id": 1,
    "schedule_event": "项目周会"
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `undone` | boolean | 是否撤销成功 |
| `operation.type` | string | 被撤销的操作类型：`"create"`、`"update"`、`"delete"` |
| `operation.schedule_id` | number | 涉及的日程 ID |
| `operation.schedule_event` | string | 涉及的日程标题 |

**失败（无可撤销的操作）：**

```json
{
  "undone": false,
  "error": {
    "code": "NO_RECENT_OPERATION",
    "message": "没有可以撤销的操作"
  }
}
```

#### 交互流程

```
用户：     "撤销刚才的操作"
LLM：      undo_last_operation()
模块返回： { undone: true, operation: { type: "create", schedule_event: "项目周会" } }
LLM→TTS：  "已撤销刚才创建的'项目周会'日程"
```

```
用户：     "撤回"
LLM：      undo_last_operation()
模块返回： { undone: false, error: { code: "NO_RECENT_OPERATION" } }
LLM→TTS：  "没有可以撤销的操作"
```

---

## 4. 错误码

| 错误码 | 说明 | 触发场景 |
|--------|------|----------|
| `SCHEDULE_NOT_FOUND` | 日程不存在 | update / delete 时传入无效的 schedule_id |
| `INVALID_TIME` | 时间不合法 | start_time 格式错误、end_time 早于 start_time |
| `NO_QUERY_CONDITION` | 未提供查询条件 | query_schedule 所有筛选参数均为 null |
| `ALREADY_CANCELLED` | 日程已取消 | 对已取消的日程执行 update |
| `NO_RECENT_OPERATION` | 无可撤销的操作 | 最近没有可撤销的操作记录 |
| `INTERNAL_ERROR` | 内部错误 | 存储读写异常等 |

---

## 5. 完整交互时序示例

### 场景 A：创建日程 + LLM 组合调用（需要提醒）

```
用户：     "明天上午十点开会，提前 15 分钟提醒我"
LLM 判断：需要创建日程 + 需要设置提醒
  ↓
LLM：     create_schedule(event="开会", start_time="2026-07-28T10:00:00+08:00")
日程返回： { schedule_id: 1, schedule: {...}, conflicts: [] }
  ↓
LLM：     add_reminder(target_description="开会",
                       target_time="2026-07-28T10:00:00+08:00",
                       advance_minutes=15)
提醒返回： { reminder_id: 1, reminder: {...} }
  ↓
LLM：     update_schedule(schedule_id=1, reminder_id=[1])
  ↓
LLM→TTS：  "好的，已添加明天上午十点开会，提前 15 分钟提醒你"
```

### 场景 B：创建日程（仅备忘，无提醒）

```
用户：     "我车停在 A130"
LLM 判断：仅备忘 → 只调日程模块
LLM：     create_schedule(event="车停在 A130", start_time="2026-07-27T20:00:00+08:00")
模块返回： { schedule_id: 2, schedule: {...}, conflicts: [] }
LLM→TTS：  "好的，已记下车停在 A130"
```

### 场景 C：模糊查询 + 二次确认 + 修改

```
用户：     "把那个开会改到下午三点"
LLM：     query_schedule(keyword="开会")
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

### 场景 D：创建 + 冲突处理

```
用户：     "明天上午十点开会"
LLM：     create_schedule(event="开会", start_time="2026-07-28T10:00:00+08:00")
模块返回： { conflicts: [{ event: "看牙医", start_time: "...10:00..." }] }
LLM→TTS：  "明天上午十点已经有一条'看牙医'的日程，还要创建吗？"
用户：     "还是创建吧"
LLM：     create_schedule(event="开会", ..., ignore_conflict=true)
LLM→TTS：  "好的，已添加"
```

### 场景 E：删除日程

```
用户：     "删掉我的停车备忘"
LLM：     query_schedule(keyword="A130")
日程返回： { schedules: [{ schedule_id: 2, event: "车停在 A130", reminder_id: [], ... }], total: 1 }
LLM：     delete_schedule(schedule_id=2)
日程返回： { schedule_id: 2, deleted: true }
LLM→TTS：  "好的，已删除"
```

---

## 6. Mock 数据参考

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
    "reminder_id": null,
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

### mock: undo_last_operation 成功

```json
{
  "undone": true,
  "operation": {
    "type": "create",
    "schedule_id": 1,
    "schedule_event": "项目周会"
  }
}
```

### mock: undo_last_operation 无操作

```json
{
  "undone": false,
  "error": {
    "code": "NO_RECENT_OPERATION",
    "message": "没有可以撤销的操作"
  }
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
      "reminder_id": 1,
      "status": "active",
      "created_at": "2026-07-27T14:30:00+08:00",
      "updated_at": "2026-07-27T14:30:00+08:00"
    },
    {
      "schedule_id": 2,
      "event": "车停在 A130",
      "start_time": "2026-07-27T20:00:00+08:00",
      "end_time": null,
      "location": null,
      "notes": null,
      "status": "active",
      "created_at": "2026-07-27T18:00:00+08:00",
      "updated_at": "2026-07-27T18:00:00+08:00"
    }
  ],
  "total": 2
}
```

---

