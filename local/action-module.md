# 动作模块需求与设计文档 V2

本文档先定义动作模块中的提醒动作边界。提醒是一个动作；IM 也归动作模块，但 IM 的动作类型、投递、渠道、回执和身份模型暂由 IM 模块负责人补充，本文件不提前规定。

## 1. 行业调研及成熟方案

1. Google Calendar API: <https://developers.google.cn/workspace/calendar/api/concepts/events-calendars?hl=zh-cn>
2. iCalendar (RFC 5545): <https://icalendar.org/RFC-Specifications/iCalendar-RFC-5545/>
3. Microsoft Graph calendar resource: <https://learn.microsoft.com/en-us/graph/api/resources/calendar?view=graph-rest-1.0&preserve-view=true>

成熟系统通常将业务事件、动作执行和用户交互分开记录。VoiceLife 在本阶段只固定提醒动作的最小事实：

| 模块 | 权威实体 | 说明 |
| --- | --- | --- |
| 日程 | `schedule`、`recurrence_rule` | 日程内容与周期意图 |
| 调度中心 | `action_trigger` | 何时触发、是否已推迟或关闭 |
| 动作模块 | `action_execution` | 一次提醒是否已受理、执行或失败 |
| IM 动作 | 待补充 | 由 IM 模块负责人补充实体、接口和状态 |

旧对象的迁移原则如下：

| 旧对象 | 新归属 | 简化后的处理 |
| --- | --- | --- |
| `reminder_rule` | 调度中心 `trigger_rule` | 它本质上是“提前何时触发”配置 |
| `reminder_trigger` | 调度中心 `action_trigger` + 动作模块 `action_execution` | 只在模块交界处分成调度事实与执行事实 |
| `reminder instance` | 不单独保留 | 由 `action_trigger` 的 occurrence 引用和 `action_execution` 的执行事实共同表达 |
| IM 相关旧对象 | 待补充 | 不在本文件重新划定 |

## 2. 核心目标

接收调度中心的到期动作请求，幂等地执行提醒，并安全接收用户的稍后或关闭操作。

提醒动作模块负责：

- 将 `action_requested` 受理为一条 `action_execution`。
- 执行设备或语音提醒，并保存执行结果。
- 对强提醒验证动作令牌、身份、有效期与参数。
- 将 `snooze`、`dismiss` 等用户操作转交调度中心。

IM 动作的投递方式、平台能力、身份绑定、回执和失败策略留在本文档之外，见末尾待补充章节。

本模块不展开周期、不计算或修改触发时间、不改变 `action_trigger` 状态，也不直接修改日程内容。

## 3. 核心概念定义

- **`action_requested` 动作请求**
  - 调度中心发布的到期事件，包含 `event_id`、`action_trigger_id`、动作类型和最小日程上下文。
  - 它不等于动作已执行，更不等于用户已经看到提醒。

- **`action_execution` 动作执行**
  - 对一个 `action_requested` 的唯一受理记录，是动作模块的核心实体。
  - 它保存本地提醒执行结果；强提醒的用户命令标识、动作和调度结果也保存在此处。

- **`user_action` 用户动作**
  - `action_execution` 上的一组字段，不是独立实体。仅强提醒可记录 `snooze` 或 `dismiss`。
  - 受理后生成 `command_id` 与 `command_status`；调度中心才有权改变 `action_trigger` 的时间和状态。

- **`action_context` 动作上下文**
  - 调度中心为动作执行提供的必要内容快照，例如标题、原计划时间和展示文本。
  - 上下文不包含日程写权限、调度存储句柄或外部平台凭据。

## 4. 核心业务流程

### 4.1 到期提醒受理

1. 调度中心发布 `action_requested`，动作模块按 `event_id` 与内容指纹幂等受理。
2. 模块创建一条 `action_execution=accepted`，并保存必要的 `action_context` 快照。
3. 当前 MVP 根据 `action_kind=reminder` 执行设备或语音提醒；动作种类的新增通过代码和契约演进，不先引入可配置的动作定义体系。
4. 提醒执行成功后进入 `succeeded`；执行失败记录稳定错误并进入 `failed`。
5. 相同 `event_id` 的重放返回已有执行；相同标识但不同内容必须拒绝。

### 4.2 IM 动作

本节留白，由 IM 模块负责人补充以下内容：

- IM 动作与提醒动作的关系。
- IM 动作实体、投递实体和身份绑定。
- 平台适配、投递尝试、平台回执和重试状态。
- IM 用户动作入口、授权、SSE/HTTP 或其他传输方式。
- IM 动作与 `action_execution` 的关联及幂等边界。

### 4.3 强提醒的用户操作

1. 弱提醒不建立交互入口；只有强提醒可生成短期动作令牌。
2. 动作入口提交 `token + action + params`；模块从令牌解析内部执行、触发和预期身份，避免暴露内部 ID。
3. 模块重新校验令牌、身份、有效窗口、动作类型与 snooze 参数。
4. 校验成功后，将 `user_action` 与稳定 `command_id` 写入对应 `action_execution`，并向调度中心发送 `SnoozeActionTrigger` 或 `DismissActionTrigger`。
5. 动作模块直接使用调度中心命令的返回值更新 `command_status`；失败或过期时如实保留结果，不能宣称提醒已经推迟或关闭。

### 4.4 日程变更、过期与恢复

1. 日程取消前尚未发布的触发由调度中心阻止，不会生成新的动作执行。
2. 已受理的本地提醒按执行状态结束；动作模块不回写日程或周期规则。
3. 强提醒窗口超时后，动作模块将未完成的用户命令标记为 `expired`。
4. 重启后从 `action_execution` 记录恢复待处理工作；`event_id` 防止重复创建事实。
5. 动作执行失败不回滚 `action_trigger`，也不改变日程状态。

### 4.5 查询与职责边界

1. 动作模块只查询提醒执行和用户命令结果。
2. “某天有什么日程”由日程模块查询；“下次何时提醒”由调度中心查询。
3. 动作模块不保存周期规则，不提供日历视图，也不直接修改 `action_trigger`。
4. 平台 SDK、HTTP、数据库和设备句柄只存在于待补充的 Adapter 与组合根，不进入提醒领域接口。
5. IM 动作的查询和历史由 IM 模块负责人补充。

---

## 5. 模块接口

### 5.1 接口总览

| 接口 | 调用方 | 说明 |
| --- | --- | --- |
| ExecuteAction | 调度中心 | 幂等受理到期动作请求 |
| SubmitUserAction | 动作入口 | 校验强提醒用户操作并转为调度命令 |
| IM 动作接口 | 待补充 | 由 IM 模块负责人定义 |

### 5.2 接口参数

#### 5.2.1 ExecuteAction

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| event_id | string | 是 | 全局唯一、幂等 | 调度中心事件标识 |
| action_trigger_id | string | 是 | 非空 | 来源触发 |
| schedule_id | string | 是 | 不透明引用 | 关联日程 |
| action_kind | string | 是 | 当前为 `reminder` | 动作种类 |
| reminder_level | string | 是 | `weak` / `strong` | 提醒等级 |
| trigger_at | datetime | 是 | ISO 8601 | 到期时间 |
| context | object | 否 | 不含凭据 | 提醒所需内容 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| action_execution_id | string | 唯一 | 动作执行标识 |
| status | string | `accepted` / `duplicate` | 受理结果 |

#### 5.2.2 SubmitUserAction

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| token | string | 是 | 有效且不可篡改 | 服务端签发令牌 |
| action | string | 是 | `snooze` / `dismiss` | 用户选择 |
| params | object | 否 | snooze 可含 minutes | 已提交参数 |
| actor_context | object | 否 | 由具体入口提供 | 发起人身份上下文 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| action_execution_id | string | 非空 | 对应动作执行 |
| command_id | string | 可空 | 校验成功时生成 |
| status | string | `succeeded` / `failed` / `expired` / `duplicate` / `rejected` | 最终处理状态 |

### 5.3 下游契约

动作模块向调度中心发送经过身份和有效期校验的用户命令：

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| command_id | string | 是 | 全局唯一，用于幂等 |
| command_type | string | 是 | `snooze_action_trigger` / `dismiss_action_trigger` |
| action_trigger_id | string | 是 | 调度中心触发引用 |
| delay_minutes | integer | 否 | snooze 时必填 |
| requested_at | datetime | 是 | 动作模块受理时间 |
| expires_at | datetime | 是 | 强提醒交互截止时间 |

约束：

- 调度中心按 `command_id` 幂等执行，只接受仍可交互的强提醒触发。
- `SubmitUserAction` 在收到调度中心同步结果后才返回；调用方不得在返回前宣称提醒已推迟或关闭。
- IM 入口如何得到 `actor_context`、如何签发和验证 token，留待 IM 模块负责人补充。

### 5.4 状态约定

- `action_execution.status`
  - 非终态：`accepted`、`executing`、`awaiting_command_result`。
  - 终态：`succeeded`、`failed`、`expired`。
  - 允许流转：`accepted -> executing / expired`，`executing -> succeeded / failed / awaiting_command_result`，`awaiting_command_result -> succeeded / failed / expired`。

- `command_status`（`action_execution` 字段）
  - `pending`：命令尚未被调度中心确认。
  - `succeeded`、`failed`、`expired`：调度中心或有效期给出的终态。
  - 每条 `action_execution` 最多保留一个成功受理的 `command_id`。

## 6. 主要数据模型

### 6.1 `action_execution`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| id | string | 主键，唯一，非空 | 动作执行标识 |
| event_id | string | 唯一，非空 | 调度中心幂等事件标识 |
| action_trigger_id | string | 非空 | 来源调度触发 |
| schedule_id | string | 非空 | 关联日程 |
| action_kind | string | 非空，枚举 | 当前为 `reminder` |
| reminder_level | string | 非空，枚举 | `weak` / `strong` |
| context_snapshot | object | 可空 | 执行时日程内容快照 |
| status | string | 非空，枚举 | 执行状态 |
| expires_at | datetime | 可空 | 强提醒交互截止时间 |
| user_action | string | 可空 | `snooze` / `dismiss` |
| command_id | string | 可空，唯一 | 已受理的调度命令 |
| command_status | string | 可空，枚举 | `pending` / `succeeded` / `failed` / `expired` |
| command_result | object | 可空 | 调度中心结果摘要 |
| created_at | datetime | 非空 | 受理时间 |
| updated_at | datetime | 非空 | 最后更新时间 |

约束：同一 `event_id` 仅一条执行；弱提醒的 `user_action` 与 `command_id` 必须为空。

### 6.2 IM 动作数据模型

本节留白，由 IM 模块负责人补充。补充范围至少包括 IM 动作类型、身份绑定、渠道投递、投递尝试、平台回执、用户动作入口和对应状态机。
