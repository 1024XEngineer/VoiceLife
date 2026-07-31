# MCP模块需求与设计文档

本文件皆在定义 VoliceLife 项目的 MCP 模块，帮助其他模块轻松完成工具定义与发现，我看了业界的一些做法（如：OpenClaw、ClaudeCode），并结合自身业务需求，提出了一套产品需求与设计。



## 0. 参考内容

1. OpenClaw：https://github.com/openclaw/openclaw
2. ClaudeCode：https://github.com/claude-code-best/claude-code
3. 灵矽：https://linx.qiniu.com/docs/xrobot/mcp/hardware-mcp



## 1. 核心目标

为VoiceLife项目提供“动手”能力，基于语音通信（ASR-LLM-TTS）这个基础 WebSocket 完成对灵矽与设备端的工具发现与调用。



## 2. 核心概念定义

定义该模块核心实体。

- 注册器：所有工具统一通过注册器来注册
  - 注册器需要验证工具的合法性
  - 注册器保证工具名称不重复
- 工具：OpenAI 工具定义格式+实际业务逻辑
  - 工具名称：灵矽平台向设备端发起工具调用时会返回注册时上传的工具名称
  - Tool definition：约束各个模块的工具定义



## 3. 核心业务流程

MCP模块的使用方式

### 流程一：工具初始化

1. 各个业务模块完成工具的定义

   示例：

   ```js
   export const userTools = [
     {
       name: 'get_user_name',
       description: '返回当前用户名称',
       inputSchema: {
         type: 'object',
         properties: {},
         additionalProperties: false,
       },
       async handler() {
         return '用户是大哥大';
       },
     },
   ];
   
   ```

2. 通过注册器完成工具的注册

   示例：

   ```js
   import { userTools } from '../tools/user/index.js';
   
   const toolGroups = [userTools];
   
   export function createToolRegistry() {
     const registry = new Map();
   
     for (const group of toolGroups) {
       for (const tool of group) {
         if (registry.has(tool.name)) {
           throw new Error('重复的工具名: ' + tool.name);
         }
         registry.set(tool.name, tool);
       }
     }
   
     return registry;
   }
   ```

3. 向灵矽平台发送工具列表

   示例：

   ```javascript
   if (method === 'tools/list') {
         sendMcp({
           jsonrpc: '2.0',
           id: payload.id,
           result: {
             tools: [...registry.values()].map(({ name, description, inputSchema }) => ({
               name,
               description,
               inputSchema,
             })),
           },
         });
         return true;
   }
   ```

   

### 流程二：工具回调

在后续用户与灵矽平台进行交互需要调用工具时的系列流程

1. 需要调工具了，灵矽平台通过 WebSocket 发送消息过来，消息中含有注册时上传的工具名称，通过工具名称确认需要调用的工具，完成调用，并将结果发送给灵矽平台。

   示例：

   ```javascript
   if (method === 'tools/call') {
         const name = payload.params?.name;
         const tool = registry.get(name);
         if (!tool) {
           sendMcp({
             jsonrpc: '2.0',
             id: payload.id,
             error: { code: -32601, message: 'unknown tool' },
           });
           return true;
         }
   
         const result = await tool.handler(payload.params?.arguments ?? {});
         sendMcp({
           jsonrpc: '2.0',
           id: payload.id,
           result: {
             content: [{ type: 'text', text: String(result) }],
             isError: false,
           },
         });
         return true;
       }
   ```



## 4.接口设计

### 接口总览

register_tool——注册一个工具定义

get_tool——根据工具名称查询已注册的工具

list_tools——获取全部已注册的工具

### 接口详情

#### 1）注册工具

**工具描述：**注册一个工具定义，并将其加入工具注册中心。注册成功后，该工具可以被查询、批量导出，并用于生成发送给模型的工具列表。

入参：

**name（String，必填）：**工具名称。建议使用命名空间格式，注册中心中不能存在同名工具。

**description（String，必填）：**工具功能描述，用于说明工具的用途，并会发送给模型。

**input（Object，可选）：**工具入参定义。每个字段需要包含字段名称、字段类型、是否必填、默认值和字段描述等信息。

**handler（Function，必填）：**模型调用回调函数

#### 2）查询工具

**工具描述：**根据工具名称查询已经注册的工具定义。

入参：

**name（String，必填）：**工具名称。

出参：

**tool（ToolDefinition，对象）：**查询到的工具定义。未找到时为空。

**found（Boolean）：**是否找到对应工具。

#### 3）获取工具列表

**工具描述：**获取工具注册中心中当前已经注册的全部工具。

入参：

无。

出参：

**tools（ToolDefinition[]）：**已经注册的工具定义列表。

**total（Integer）：**工具总数。





## 5. MCP Tool

### 日程 Tools

#### create_schedule

创建一条日程。如果时间冲突且未忽略冲突，则仅返回冲突列表且不创建。

内部编排：

1. 创建 `schedule` 主记录。
2. 若 `start_time` 不为空，则内部调用 `RegisterTimerTask(schedule_id, start_at)` 创建对应 `timer_task`。
3. 若本次创建同时包含 `recurrence_rule` 或 `reminder_config`，则继续内部调用 `UpdateTimerTask(task_id, schedule_id, start_at, recurrence_rule, reminder_config, change_scope=all)` 补全调度规则。
4. 若仅创建无时间语义的普通记录，则不编排 `timingtask`。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| event | string | 是 | 事件标题 |
| start_time | datetime \| null | 否 | 开始时间 |
| end_time | datetime \| null | 否 | 结束时间/开始时间+持续时间=结束时间 |
| location | string \| null | 否 | 地点 |
| notes | string \| null | 否 | 备注 |
| recurrence_rule | object \| null | 否 | 周期规则；不传表示单次事项 |
| reminder_config | object \| null | 否 | 提醒配置；不传表示仅记录日程、不触发提醒 |
| ignore_conflict | boolean | 否 | 是否忽略与其他日程的时间冲突，默认 false |

**返回：**

返回结构化 JSON 数据。

| 字段 | 类型 | 说明 |
|---|---|---|
| created | boolean | 是否成功创建日程 |
| schedule | Schedule \| null | 创建成功后的完整日程；未创建时为 null |
| task_id | string \| null | 若已同步到 `timingtask`，则返回对应任务 ID |
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
    "notes": null,
    "recurrence_rule": null,
    "reminder_config": {
      "weak_offsets": [-30],
      "strong": {
        "enabled": true,
        "can_snooze": true,
        "snooze_interval_minutes": 10
      }
    },
    "status": "active"
  },
  "task_id": "task_12",
  "conflicts": [],
  "error": null
}
```

存在时间冲突且未忽略冲突时，不创建日程：

```json
{
  "created": false,
  "schedule": null,
  "task_id": null,
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
  "task_id": null,
  "conflicts": [],
  "error": {
    "code": "INVALID_TIME",
    "message": "结束时间不能早于开始时间"
  }
}
```

#### query_schedule

根据日程 ID、关键词或时间范围查询日程主记录。多个查询条件之间为 AND 关系，结果按开始时间升序排列。不传任何参数默认查询所有日程。

说明：

- 该 Tool 返回 `schedule` 主记录，适合查看“用户保存了哪些事项”。
- 若用户问“明天有什么安排”“下周五有哪些事情”，应优先调用 `query_calendar_view`，由内部调度层按时间范围展开周期事项与单次例外。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number \| null | 否 | 日程 ID；提供后按 ID 精确查询 |
| keyword | string \| null | 否 | 事件标题关键词，支持模糊匹配 |
| start_from | datetime \| null | 否 | 开始时间范围的下限 |
| start_to | datetime \| null | 否 | 开始时间范围的上限 |
| status | string \| null | 否 | 状态筛选，默认 active；all 表示全部状态；cancelled 已取消日程 |
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
      "reminder_config": {
        "weak_offsets": [-10, -30],
        "strong": {
          "enabled": true,
          "can_snooze": true
        }
      },
      "status": "active"
    }
  ],
  "total": 1,
  "error": null
}
```

#### update_schedule

修改一条已有日程。调用前应先查询并确定目标日程的 `schedule_id`。如果时间冲突且未忽略冲突，则仅返回冲突列表且不修改任何字段；另外修改时只调整部分字段的话，不需要调整的字段值为原始值（原来是空现在还是空，原来是什么值，现在还是什么值）。

内部编排：

1. 更新 `schedule` 主记录。
2. 若该日程已有对应 `timer_task`，则按本次请求内部调用 `UpdateTimerTask` 同步 `start_time`、`recurrence_rule`、`reminder_config`。
3. 若该日程此前没有 `timer_task`，但本次更新后具备时间语义，则先调用 `RegisterTimerTask(schedule_id, start_at)`，再调用 `UpdateTimerTask(...)` 补全规则。
4. 若本次更新移除了时间语义或明确关闭提醒，则可在内部编排 `CancelTimerTask(change_scope=all)` 终止后续触发。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number | 是 | 要修改的日程 ID |
| event | string \| null | 否 | 新的事件标题 |
| start_time | datetime \| null | 否 | 新的开始时间 |
| end_time | datetime \| null | 否 | 新的结束时间 |
| location | string \| null | 否 | 新的地点 |
| notes | string \| null | 否 | 新的备注 |
| recurrence_rule | object \| null | 否 | 新的周期规则；传 null 可表示改为单次事项 |
| reminder_config | object \| null | 否 | 新的提醒配置；传 null 可表示移除提醒 |
| change_scope | string \| null | 否 | 修改范围：`single` / `future` / `all`；默认 `all` |
| target_occurrence_at | datetime \| null | 否 | 修改“本次”时的目标 occurrence 时间 |
| effective_from | datetime \| null | 否 | 修改“本次及以后”时的生效时间 |
| ignore_conflict | boolean | 否 | 是否忽略时间冲突，默认 false |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| updated | boolean | 是否成功修改日程 |
| schedule | Schedule \| null | 修改后的完整日程 |
| task_id | string \| null | 若同步了 `timingtask`，则返回对应任务 ID |
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
    "recurrence_rule": {
      "frequency": "week",
      "interval": 1,
      "start_at": "2026-07-29T15:00:00+08:00",
      "timezone": "+08:00",
      "by_weekdays": ["wed"],
      "end_type": "none"
    },
    "reminder_config": {
      "weak_offsets": [-30],
      "strong": {
        "enabled": true,
        "can_snooze": true,
        "max_snooze_count": 2
      }
    },
    "status": "active"
  },
  "task_id": "task_12",
  "conflicts": [],
  "error": null
}
```

#### delete_schedule

删除/取消一条日程。调用前应先查询并确定目标日程的 `schedule_id`。

内部编排：

1. 更新或删除 `schedule` 主记录。
2. 若该日程存在对应 `timer_task`，则内部调用 `CancelTimerTask` 停止后续触发。
3. `single` / `future` / `all` 等范围语义由 MCP Tool 转换后传给 `timingtask`，避免 Agent 直接操作底层调度对象。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number | 是 | 要删除的日程 ID |
| change_scope | string \| null | 否 | 删除范围：`single` / `future` / `all`；默认 `all` |
| target_occurrence_at | datetime \| null | 否 | 删除“本次”时的目标 occurrence 时间 |
| effective_from | datetime \| null | 否 | 删除“本次及以后”时的生效时间 |

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

#### query_calendar_view

按时间范围查询用户可见安排。该 Tool 面向“明天有什么安排”“下周五有哪些事情”这类用户问题，内部调用 `ListCalendarView` 展开周期事项并叠加单次例外。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| range_start | datetime | 是 | 查询开始时间 |
| range_end | datetime | 是 | 查询结束时间 |
| schedule_id | number \| null | 否 | 指定某条日程；为空时查询当前用户范围内全部日程 |
| status | string \| null | 否 | 状态筛选 |
| limit | number \| null | 否 | 返回条数，默认 20，最大 100 |
| offset | number \| null | 否 | 分页偏移量，默认 0 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| occurrences | Occurrence[] | 查询时间范围内用户可见的安排列表 |
| total | number | 符合条件的安排总数 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "occurrences": [
    {
      "occurrence_id": "task_12#2026-08-07T20:00:00+08:00",
      "schedule_id": 12,
      "task_id": "task_12",
      "instance_id": null,
      "event": "项目周会",
      "planned_start_at": "2026-08-07T20:00:00+08:00",
      "planned_end_at": "2026-08-07T21:00:00+08:00",
      "status": "pending",
      "is_recurring": true,
      "is_exception": false
    }
  ],
  "total": 1,
  "error": null
}
```

#### query_recent_operations

查询当前用户最近 15 分钟内可撤销的日程操作（限制：仅允许撤销最近 10 条操作，不可调整）。如需撤销应先调用该 Tool，找到用户想撤销的操作记录。

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
      "operation_id": 100,
      "type": "update",
      "schedule_id": 1,
      "schedule_event": "项目周会",
      "operated_at": "2026-07-27T15:05:00+08:00",
      "previous": {  // 本次操作前的数据
        "schedule_id": 1,
        "event": "项目周会",
        "start_time": "2026-07-28T10:00:00+08:00",
        "end_time": "2026-07-28T11:00:00+08:00",
        "location": "301会议室",
        "notes": null,
        "reminder_config": {
          "weak_offsets": [-10],
          "strong": {
            "enabled": true,
            "can_snooze": true
          }
        },
        "status": "active",
        "created_at": "2026-07-27T14:30:00+08:00",
        "updated_at": "2026-07-27T14:30:00+08:00"
      }
    },
    {
      "operation_id": 101,
      "type": "create",
      "schedule_id": 12,
      "schedule_event": "项目周会",
      "operated_at": "2026-07-29T09:50:00+08:00",
      "previous": null   // 新建日程前没有该日程的相关数据
    }
  ],
  "error": null
}
```

#### undo_operation

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
    "reminder_config": {
      "weak_offsets": [-10],
      "strong": {
        "enabled": true,
        "can_snooze": true
      }
    },
    "status": "active"
  },
  "error": null
}
```

### 提醒 Tools

说明：

- 对于绑定 `schedule` 的提醒，本节 Tool 对应的是 `timingtask` 的 reminder rule / reminder trigger 能力。
- “修改提醒策略”属于配置态，内部会编排 `UpsertReminderRules` / `DeleteReminderRule`。
- “推迟 / 关闭强提醒”属于运行态，内部会编排 `ListReminderTriggers`、`SnoozeReminderTrigger`、`DismissReminderTrigger`。

#### update_schedule_reminders

为一条已有日程创建、修改或关闭提醒策略。该 Tool 面向“再提前 30 分钟提醒我一次”“取消 10 分钟前提醒”“把强提醒改成可推迟 2 次”这类需求。

内部编排：

1. 先根据 `schedule_id` 查询并确认对应的 `task_id`。
2. 根据请求内容将提醒配置转换成一组 `reminder_rule`。
3. 内部调用 `UpsertReminderRules(task_id, rules)` 创建或更新提醒规则。
4. 若请求要求关闭某条已有提醒规则，则内部调用 `DeleteReminderRule(reminder_rule_id)`。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number | 是 | 要修改提醒策略的日程 ID |
| change_scope | string \| null | 否 | 生效范围：`future` / `all`；默认 `all` |
| weak_reminders | array<object> \| null | 否 | 弱提醒策略列表；空数组可表示移除全部弱提醒 |
| strong_reminder | object \| null | 否 | 强提醒策略；传 null 可表示关闭强提醒 |

`weak_reminders` 子项建议字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_rule_id | string \| null | 否 | 更新已有弱提醒规则时传入 |
| offset_minutes | number | 是 | 相对事件开始时间的偏移分钟，通常为负数 |
| enabled | boolean | 否 | 是否启用，默认 true |
| channel | string \| null | 否 | 提醒渠道，如 `voice` / `im` |

`strong_reminder` 建议字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_rule_id | string \| null | 否 | 更新已有强提醒规则时传入 |
| enabled | boolean | 否 | 是否启用，默认 true |
| can_snooze | boolean | 否 | 是否允许推迟，默认 true |
| max_snooze_count | number \| null | 否 | 最大推迟次数 |
| snooze_interval_minutes | number \| null | 否 | 默认推迟间隔 |
| channel | string \| null | 否 | 提醒渠道 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| updated | boolean | 是否成功更新提醒策略 |
| schedule_id | number | 日程 ID |
| task_id | string \| null | 对应定时任务 ID |
| reminder_rules | ReminderRule[] | 更新后的提醒规则列表 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "updated": true,
  "schedule_id": 12,
  "task_id": "task_12",
  "reminder_rules": [
    {
      "reminder_rule_id": "rr_weak_10",
      "reminder_type": "weak",
      "offset_minutes": -10,
      "enabled": true,
      "channel": "voice",
      "status": "active"
    },
    {
      "reminder_rule_id": "rr_weak_30",
      "reminder_type": "weak",
      "offset_minutes": -30,
      "enabled": true,
      "channel": "im",
      "status": "active"
    },
    {
      "reminder_rule_id": "rr_strong_0",
      "reminder_type": "strong",
      "offset_minutes": 0,
      "enabled": true,
      "can_snooze": true,
      "max_snooze_count": 2,
      "snooze_interval_minutes": 10,
      "status": "active"
    }
  ],
  "error": null
}
```

#### query_active_strong_reminders

查询当前可响应的强提醒触发。适用于用户说“把刚才那个提醒推迟十分钟”“关闭这个提醒”之前，让 Agent 先定位当前正在触发或刚触发的强提醒。

内部编排：

1. 内部调用 `ListReminderTriggers`，筛选 `reminder_type=strong`。
2. 默认优先返回 `status in (triggered, snoozed)` 的提醒触发。
3. 若传入 `schedule_id` 或 `keyword`，则进一步结合 `schedule` 主数据做筛选。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| schedule_id | number \| null | 否 | 指定某条日程对应的强提醒 |
| keyword | string \| null | 否 | 事件标题关键词 |
| limit | number \| null | 否 | 返回条数，默认 10，最大 20 |
| offset | number \| null | 否 | 分页偏移量，默认 0 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| strong_reminders | StrongReminderTrigger[] | 当前可响应的强提醒触发列表 |
| total | number | 符合条件的提醒总数 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "strong_reminders": [
    {
      "reminder_trigger_id": "rtg_9001",
      "schedule_id": 12,
      "task_id": "task_12",
      "instance_id": "ins_20260729_1000",
      "event": "项目周会",
      "planned_trigger_at": "2026-07-29T10:00:00+08:00",
      "actual_trigger_at": "2026-07-29T10:00:00+08:00",
      "status": "triggered",
      "can_snooze": true,
      "snooze_count": 0
    }
  ],
  "total": 1,
  "error": null
}
```

#### snooze_strong_reminder

推迟当前正在触发的强提醒。仅适用于 `timingtask` 中 `reminder_type=strong` 且允许 snooze 的触发。

内部编排：

1. 若调用方未直接提供 `reminder_trigger_id`，可先调用 `query_active_strong_reminders` 定位目标提醒。
2. 内部调用 `SnoozeReminderTrigger(reminder_trigger_id, delay_minutes)`。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_trigger_id | string | 是 | 要推迟的强提醒触发 ID |
| snooze_minutes | number \| null | 否 | 推迟分钟数；未指定时使用该触发对应规则的默认推迟间隔 |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| reminder_trigger_id | string | 被推迟的强提醒触发 ID |
| snoozed | boolean | 是否成功推迟 |
| next_trigger_at | datetime \| null | 推迟后的下一次触发时间 |
| snooze_count | number | 推迟后的累计次数 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "reminder_trigger_id": "rtg_9001",
  "snoozed": true,
  "next_trigger_at": "2026-07-29T10:20:00+08:00",
  "snooze_count": 1,
  "error": null
}
```

#### dismiss_strong_reminder

关闭当前正在触发或已 snooze 的强提醒，关闭后不再继续播放。

内部编排：

1. 若调用方未直接提供 `reminder_trigger_id`，可先调用 `query_active_strong_reminders` 定位目标提醒。
2. 内部调用 `DismissReminderTrigger(reminder_trigger_id)`。

**参数：**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| reminder_trigger_id | string | 是 | 要关闭的强提醒触发 ID |

**返回：**

| 字段 | 类型 | 说明 |
|---|---|---|
| reminder_trigger_id | string | 被关闭的强提醒触发 ID |
| dismissed | boolean | 是否成功关闭 |
| error | Error \| null | 调用失败时的错误信息；无错误时为 null |

```json
{
  "reminder_trigger_id": "rtg_9001",
  "dismissed": true,
  "error": null
}
```
