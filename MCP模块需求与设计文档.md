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

### 1. 日程管理相关Tool

#### 1）创建日程

**工具描述：**创建一条日程。如果时间冲突且未忽略冲突，则仅返回冲突列表且不创建。

**入参：**

- **event(String，必填）:** 事件标题
- **start_time(String， 可选）：**事件开始时间
- **end_time(datetime，可选）：**事件结束时间
- **location（datetime，可选）：**事件地点

- **notes（String，可选）：**事件备注
- **ignore_conflict(Boolean, 可选，默认 False）：**是否忽略与其他日程的时间冲突

**出参：**

**message（String）：**对此次创建的一些说明，比如：”created successful“。

**schedule（Schedule 实体对象）：**创建完成后的 Schedule 实体对象。

**conflicts（Schedule[]）：**创建成功时此字段为空，日程冲突时为冲突日程实体。

**error（String）：**无错误时为空。和 message 字段的区别是，error 字段更加突出，message 字段适合常规信息。

#### 2）查询日程

**工具描述：**根据日程 ID、关键词或时间范围查询日程。多个查询条件之间为 AND 关系，结果按开始时间升序排列。不传任何参数默认查询所有日程。

**入参：**

- **schedule_id(Number，可选）：**日程 ID，提供后按 ID 精确查询
- **keyword(String，可选）：**事件标题关键词，支持模糊匹配
- **start_from(datetime，可选）：**开始时间范围的下限
- **start_to(datetime，可选）：**开始时间范围的上限
- **status(String，可选，默认 active）：**状态筛选；all 表示全部状态，cancelled 表示已取消日程
- **limit(Number，可选，默认 10，最大 50）：**返回条数
- **offset(Number，可选，默认 0）：**分页偏移量

**出参：**

**schedules（Schedule[]）：**符合条件的日程列表。

**total（Number）：**符合条件的日程总数，不受 limit 和 offset 影响。

**error（String）：**调用失败时的错误信息；无错误时为空。

#### 3）修改日程

**工具描述：**修改一条已有日程。调用前应先查询并确定目标日程的 schedule_id。如果时间冲突且未忽略冲突，则仅返回冲突列表且不修改任何字段。

**入参：**

- **schedule_id(Number，必填）：**要修改的日程 ID
- **event(String，可选）：**新的事件标题
- **start_time(datetime，可选）：**新的开始时间
- **end_time(datetime，可选）：**新的结束时间
- **location(String，可选）：**新的地点
- **notes(String，可选）：**新的备注
- **reminder_id(Number，可选）：**关联的提醒 ID
- **ignore_conflict(Boolean，可选，默认 False）：**是否忽略时间冲突

**出参：**

**message（String）：**对此次修改的一些说明。

**schedule（Schedule 实体对象）：**修改后的完整日程；未修改时为空。

**conflicts（Schedule[]）：**修改后发生冲突的日程；无冲突时为空数组。

**error（String）：**调用失败时的错误信息；无错误时为空。

#### 4）删除日程

**工具描述：**删除/取消一条日程。调用前应先查询并确定目标日程的 schedule_id。该接口不会自动删除关联提醒。

**入参：**

- **schedule_id(Number，必填）：**要删除的日程 ID

**出参：**

**schedule_id（Number）：**被删除的日程 ID。

**deleted（Boolean）：**是否成功删除日程。

**error（String）：**调用失败时的错误信息；无错误时为空。

#### 5）查询最近日程操作

**工具描述：**查询当前用户最近 10 条日程操作。如需撤销，应先调用该接口找到用户想撤销的操作记录。

**入参：**

无。

**出参：**

**operations（Operation[]）：**可撤销的操作记录（包含操作和日程状态），按操作时间倒序排列。

**error（String）：**调用失败时的错误信息；无错误时为空。

#### 6）撤销日程操作

**工具描述：**撤销当前用户最近 15 分钟内指定的一条日程创建、修改或删除操作。

**入参：**

- **operation_id(Number，必填）：**要撤销的操作记录 ID，由 query_recent_operations 接口获取

**出参：**

**undone（Boolean）：**是否成功撤销。

**operation（Operation 实体对象）：**被撤销的操作信息（撤销前的状态）；无操作时为空。

**schedule（Schedule 实体对象）：**撤销完成后的日程（撤销后的状态）；撤销创建操作时为空。

**error（String）：**操作不存在、已过期或调用失败时的错误信息；无错误时为空。






## 提醒 Tools

说明：

- 对于绑定 `schedule` 的提醒，本节 Tool 对应的是 `timingtask` 的 reminder rule / reminder trigger 能力。
- “修改提醒策略”属于配置态，内部会编排 `UpsertReminderRules` / `DeleteReminderRule`。
- “推迟 / 关闭强提醒”属于运行态，内部会编排 `ListReminderTriggers`、`SnoozeReminderTrigger`、`DismissReminderTrigger`。

### update_schedule_reminders

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

### query_active_strong_reminders

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

### snooze_strong_reminder

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

### dismiss_strong_reminder

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

