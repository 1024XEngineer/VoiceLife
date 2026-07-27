# 提醒模块 - 接口文档

## 1. 概述

### 1.1 模块定位

提醒模块运行在硬件设备本地，负责管理与执行所有提醒任务。它是一个完全独立的服务——提醒本身不持有任何外部引用（不存储 target_type、target_id），只关心"什么时间、说什么话"。外部模块（如日程）通过持有 `reminder_id` 来关联提醒，关联关系由 LLM 在调用侧维护。

对外暴露一组函数接口，供上层 LLM Agent 通过 Function Calling 调用。提醒触发时，通过系统事件通知 TTS 模块进行语音播报。

### 1.2 职责边界

```
LLM Agent
  │
  ├── 提醒模块（本模块）
  │   ├── 弱提醒：提前 N 分钟触发，播一次即止
  │   ├── 强提醒：到点触发，支持重试循环、推迟、关闭
  │   ├── 状态机管理
  │   └── 触发事件 → TTS 播报
  │
  ├── 日程模块（独立，不耦合）
  └── 其他模块 ...
```

**本模块负责：**
- 弱提醒的调度与执行（仅播放一次，无交互）
- 强提醒的调度、重试循环、推迟（snooze）、关闭（dismiss）
- 提醒触发时产生系统事件，推送 TTS 播报文本

**本模块不负责：**
- 语音识别（ASR）
- 自然语言意图解析（由 LLM 完成）
- 日程的 CRUD（由日程模块负责）
- TTS 语音合成（本模块仅输出文本，TTS 模块负责播报）

### 1.3 提醒模型概述

```
      ┌── 弱提醒 ── 提前 N 分钟触发，播一次 → completed
      │
提醒 ─┤
      │                   ┌─ 无回应 ×3 ──→ completed
      └── 强提醒 ── 到点触发
                          ├─ "我知道了" ──→ dismissed
                          ├─ "等会儿"    ──→ 推迟 interval 再播
                          └─ "N分钟后"   ──→ 推迟 N 分钟再播
```

---

## 2. 数据模型

### 2.1 Reminder（提醒）

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | number | 自动生成 | 自增主键，全局唯一 |
| `target_description` | string | ✅ | 目标的自然语言描述，用于生成 TTS 播报文本，如 `"项目周会"` |
| `target_time` | string (ISO 8601) | ✅ | 目标事件的发生时刻，如 `"2026-07-28T10:00:00+08:00"`。弱提醒以此时间向前偏移；强提醒在此刻触发 |
| `advance_minutes` | number \| null | ✅ | 弱提醒的提前分钟数。默认 `15`，用户可通过 `update_reminder` 调整 |
| `strong_enabled` | boolean | ✅ | 是否启用强提醒。`false` 表示仅弱提醒，到点不触发 |
| `max_retries` | number | 自动 | 强提醒最大播放次数，默认 `3` |
| `retry_interval_minutes` | number | 自动 | 无用户回应时的重试间隔，默认 `10` 分钟 |
| `status` | string | 自动 | 提醒整体状态：`"pending"` \| `"active"` \| `"completed"` \| `"cancelled"` |
| `strong_state` | StrongState \| null | 自动 | 强提醒运行时状态。无强提醒或未激活时为 null |
| `created_at` | string (ISO 8601) | 自动 | 创建时间 |
| `updated_at` | string (ISO 8601) | 自动 | 最后修改时间 |

### 2.2 示例

**示例——有弱提醒 + 强提醒，pending 状态：**

```json
{
  "reminder_id": 1,
  "target_description": "项目周会",
  "target_time": "2026-07-28T10:00:00+08:00",
  "advance_minutes": 15,
  "strong_enabled": true,
  "max_retries": 3,
  "retry_interval_minutes": 10,
  "status": "pending",
  "strong_state": null,
  "created_at": "2026-07-27T14:30:00+08:00",
  "updated_at": "2026-07-27T14:30:00+08:00"
}
```

**示例——仅备忘提醒（无弱提醒、无强提醒），注意这种场景不应调用提醒模块：**

仅备忘、不需要提醒的日程——LLM 只调日程模块，不调提醒模块。

---

## 3. 弱提醒行为规范

弱提醒是在目标时间之前**只播放一次**的提醒，默认提前 15 分钟触发，用户可通过 `update_reminder` 调整提前量或关闭弱提醒。

**行为：**
- 在 `target_time - advance_minutes` 时刻，播放一次 TTS，然后该弱提醒即告完成。
- 仅播放一次，无重试、无推迟、无交互。
- 不影响强提醒的状态。
- `advance_minutes` 为 `null` 时不启用弱提醒，仅保留强提醒。

**TTS 示例（提前 15 分钟）：** `"提醒：你的'项目周会'将在 15 分钟后开始"`
**TTS 示例（到点）：** `"提醒：你的'项目周会'现在开始了"`（此时 `advance_minutes=0`，由强提醒逻辑接管更合适，建议设 `advance_minutes=null` 并 `strong_enabled=true`）

---

## 4. 强提醒行为规范

强提醒在 `target_time` 时刻触发，进入重试循环。

### 4.1 状态机

```
target_time 到点
  │
  ▼
triggered（retry_count=1，TTS 播报）
  │
  ├─ 无回应，等 interval ──→ triggered（retry_count=2）
  │    ├─ 无回应 ──→ triggered（retry_count=3）
  │    │    ├─ 无回应 ──→ completed（终止，不再提醒）
  │    │    ├─ "我知道了" ──→ dismissed → completed
  │    │    ├─ "等会儿"（已达上限）──→ 等 interval → triggered（retry_count 不变，最后一次）
  │    │    └─ "N分钟后" ──→ snoozed → 等 N 分钟 → triggered（retry_count++）
  │    │
  │    ├─ "我知道了" ──→ dismissed → completed
  │    ├─ "等会儿"（未达上限）──→ 等 interval → triggered（retry_count++）
  │    └─ "N分钟后" ──→ snoozed → 等 N 分钟 → triggered（retry_count++，之后恢复 interval）
  │
  ├─ "我知道了" ──→ dismissed → completed
  ├─ "等会儿"（未达上限）──→ 等 interval → triggered（retry_count++）
  └─ "N分钟后" ──→ snoozed → 等 N 分钟 → triggered（retry_count++，之后恢复 interval）
```

### 4.2 用户行为对照表

| 用户行为 | 对应接口 | retry_count < max_retries | retry_count ≥ max_retries |
|----------|---------|---------------------------|---------------------------|
| 无回应 | - | 等 `retry_interval_minutes` 后 `retry_count++`，继续播放 | 转入 `completed`，终止 |
| "我知道了" | `dismiss_reminder` | 转入 `dismissed`，永久停止 | 转入 `dismissed`，永久停止 |
| "等会儿再提醒" | `snooze_reminder`（不传 `snooze_minutes`） | 等 `retry_interval_minutes` 后 `retry_count++`，继续播放 | 等 `retry_interval_minutes` 后播放**最后一次**，`retry_count` 不变，再无回应则终止 |
| "N 分钟后再提醒" | `snooze_reminder(snooze_minutes=N)` | 等 N 分钟后触发，`retry_count++`，之后恢复 `retry_interval_minutes` 间隔 | 等 N 分钟后触发，`retry_count++`，之后恢复 `retry_interval_minutes` 间隔 |

---

## 5. 接口定义

### 通用约定

**时间格式：** 所有时间字段统一使用 `string` 类型 + ISO 8601 带时区偏移，例如 `"2026-07-28T10:00:00+08:00"`。

**返回值结构：** 成功时包含业务数据，失败时包含错误信息：

```json
// 成功
{ "reminder_id": 1, "reminder": { ... } }

// 失败
{
  "error": {
    "code": "REMINDER_NOT_FOUND",
    "message": "未找到 ID 为 5 的提醒"
  }
}
```

---

### 5.1 add_reminder

为一个目标实体挂载提醒。

**触发场景：** 用户需要设置提醒时调用。提醒模块不关心调用方是什么（日程、闹钟、待办等均可）。LLM 在 `add_reminder` 成功后，需将返回的 `reminder_id` 回写到调用方的关联字段中（如日程的 `reminder_id`）。

#### 入参

```json
{
  "target_description": "项目周会",
  "target_time": "2026-07-28T10:00:00+08:00",
  "advance_minutes": 15,
  "strong_enabled": true,
  "max_retries": 3,
  "retry_interval_minutes": 10
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `target_description` | string | ✅ | 提醒事项的描述，用于生成 TTS 播报文本 |
| `target_time` | string (ISO 8601) | ✅ | 提醒的目标时刻 |
| `advance_minutes` | number \| null | ✅ | 弱提醒提前分钟数。**默认 `15`**，null 或 0 到点提醒 |
| `strong_enabled` | boolean | ✅ | 是否启用强提醒。`true` 时 `max_retries` 和 `retry_interval_minutes` 生效 |
| `max_retries` | number | 否 | 强提醒最大播放次数，默认 `3` |
| `retry_interval_minutes` | number | 否 | 无回应时的重试间隔，默认 `10` |

**LLM 调用指引：**

| 用户表述 | advance_minutes | strong_enabled |
|---|---|---|
| "提前 15 分钟提醒我开会" | `15` | `true` |
| "提醒我开会"（未指定时间） | `15`（默认值） | `true` |
| "到点提醒我" | `null` | `true` |
| "提前 5 分钟提醒我" | `5` | `true` |
| "提前半小时提醒我" | `30` | `true` |

#### 出参

```json
{
  "reminder_id": 1,
  "reminder": { "...完整 Reminder 对象..." }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `reminder_id` | number | 创建的提醒 ID |
| `reminder` | Reminder | 完整的提醒对象 |

---

### 5.2 update_reminder

修改一个已有提醒的配置。

**触发场景：** "把开会的提前提醒改成 20 分钟"、"开会不用强提醒了"

> **注意：** 此接口修改的是提醒的**配置参数**（advance_minutes、strong_enabled 等），不是运行时状态。运行时推迟/关闭请用 `snooze_reminder` / `dismiss_reminder`。

#### 入参

```json
{
  "reminder_id": 1,
  "advance_minutes": 20,
  "strong_enabled": null,
  "max_retries": null,
  "retry_interval_minutes": null
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 目标提醒 ID |
| `advance_minutes` | number \| null | 否 | 新的弱提醒提前分钟数。传 `0` 或 `null` 表示关闭弱提醒。传 null（不传该字段）表示不修改 |
| `strong_enabled` | boolean \| null | 否 | 新的强提醒开关。传 null 表示不修改 |
| `max_retries` | number \| null | 否 | 新的最大重试次数。传 null 表示不修改 |
| `retry_interval_minutes` | number \| null | 否 | 新的重试间隔。传 null 表示不修改 |

**部分更新规则：** 仅更新传入的非 null 字段，未传入的字段保持原值。

**副作用：** 修改生效后，尚未触发的弱提醒和强提醒按新配置执行。已触发的强提醒（`strong_state.phase=triggered`）不受配置修改影响，直到当前轮次结束。

#### 出参

```json
{
  "reminder_id": 1,
  "reminder": { "...更新后的完整 Reminder 对象..." }
}
```

---

### 5.3 remove_reminder

移除一个提醒。

**触发场景：** "不用提醒我开会了"、日程被删除时由 LLM 调用本接口清理关联提醒。

#### 入参

```json
{
  "reminder_id": 1
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 要移除的提醒 ID |

#### 出参

```json
{
  "reminder_id": 1,
  "removed": true
}
```

**注意：** 日程模块删除日程时，不会自动通知提醒模块。LLM 在收到日程模块的删除成功响应后，需要检查该日程是否有对应提醒、如有则调用此接口清理。

---

### 5.4 snooze_reminder

推迟一个当前正在触发的强提醒。

**触发场景：** 提醒正在播报时，用户说——
- "等会儿再提醒我" → 不传 `snooze_minutes`
- "20 分钟后再提醒我" → 传入 `snooze_minutes=20`

**调用前提：** 仅对 `strong_state.phase="triggered"` 的提醒有效。

#### 入参

```json
{
  "reminder_id": 1,
  "snooze_minutes": null
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 强提醒 ID |
| `snooze_minutes` | number \| null | 否 | 推迟分钟数。`null` 或不传 = "等会儿"，按 `retry_interval_minutes` 推迟；传入数值则按时长推迟 |

**行为规则：** 见 [4.2 用户行为对照表](#42-用户行为对照表)。

#### 出参

```json
{
  "reminder_id": 1,
  "next_trigger_at": "2026-07-28T10:20:00+08:00",
  "retry_count": 2,
  "snoozed": true
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `reminder_id` | number | 提醒 ID |
| `next_trigger_at` | string (ISO 8601) | 下次触发时间 |
| `retry_count` | number | 推迟后的 retry_count |
| `snoozed` | boolean | 是否推迟成功 |

---

### 5.5 dismiss_reminder

关闭一个当前正在触发的强提醒，永久停止。

**触发场景：** 提醒正在播报时，用户说"我知道了"、"关闭提醒"

**调用前提：** 仅对 `strong_state.phase="triggered"` 的提醒有效。

#### 入参

```json
{
  "reminder_id": 1
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 强提醒 ID |

#### 出参

```json
{
  "reminder_id": 1,
  "dismissed": true
}
```

---

### 5.6 query_reminder

按 ID 查询提醒。

**触发场景：** 需要查看某个提醒的当前状态（运行时 retry_count、phase 等）。

> **注意：** 查找"某条日程有哪些提醒"不再使用此接口。LLM 应直接查询日程的 `reminder_id` 字段，然后用 `query_reminder(reminder_id=X)` 逐条获取详情（或批量传入 IDs）。此接口也支持 `reminder_id` 批量查询。

#### 入参

```json
{
  "reminder_id": 1
}
```

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `reminder_id` | number | ✅ | 提醒 ID |

#### 出参

```json
{
  "reminder": {
      "reminder_id": 1,
      "target_description": "项目周会",
      "target_time": "2026-07-28T10:00:00+08:00",
      "advance_minutes": 15,
      "strong_enabled": true,
      "max_retries": 3,
      "retry_interval_minutes": 10,
      "status": "active",
      "strong_state": {
        "retry_count": 1,
        "phase": "triggered",
        "next_trigger_at": null
      },
      "created_at": "2026-07-27T14:30:00+08:00",
      "updated_at": "2026-07-27T14:30:00+08:00"
    }
  ],
}
```

---

## 6. 系统事件

提醒触发时，提醒模块通过回调/事件机制通知上层。

### 6.1 弱提醒触发事件

```json
{
  "event_type": "reminder_triggered",
  "reminder_id": 1,
  "reminder_type": "weak",
  "target_description": "项目周会",
  "target_time": "2026-07-28T10:00:00+08:00",
  "advance_minutes": 15,
  "triggered_at": "2026-07-28T09:45:00+08:00",
  "tts_message": "提醒：你的'项目周会'将在 15 分钟后开始"
}
```

### 6.2 强提醒触发事件

```json
{
  "event_type": "reminder_triggered",
  "reminder_id": 1,
  "reminder_type": "strong",
  "target_description": "项目周会",
  "target_time": "2026-07-28T10:00:00+08:00",
  "retry_count": 1,
  "max_retries": 3,
  "triggered_at": "2026-07-28T10:00:00+08:00",
  "tts_message": "提醒：你的'项目周会'现在开始了"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `event_type` | string | 固定为 `"reminder_triggered"` |
| `reminder_id` | number | 提醒 ID |
| `reminder_type` | string | `"weak"` 或 `"strong"` |
| `target_description` | string | 目标描述 |
| `target_time` | string (ISO 8601) | 目标事件时刻 |
| `advance_minutes` | number | 仅弱提醒事件包含，提前分钟数 |
| `retry_count` | number | 仅强提醒事件包含，当前第几次播放 |
| `max_retries` | number | 仅强提醒事件包含，最大播放次数 |
| `triggered_at` | string (ISO 8601) | 实际触发时间 |
| `tts_message` | string | 预生成的 TTS 播报文本 |

---

## 7. 错误码

| 错误码 | 说明 | 触发场景 |
|--------|------|----------|
| `REMINDER_NOT_FOUND` | 提醒不存在 | update / remove / snooze / dismiss 时传入无效的 reminder_id |
| `INVALID_ADVANCE_MINUTES` | 弱提醒参数不合法 | advance_minutes < 0 或超过上限（如 > 1440 分钟） |
| `INVALID_RETRY_CONFIG` | 重试配置不合法 | max_retries ≤ 0、retry_interval_minutes ≤ 0 |
| `STRONG_NOT_ACTIVE` | 强提醒未激活 | 对非 triggered 状态的强提醒调用 snooze / dismiss |
| `STRONG_NOT_ENABLED` | 强提醒未启用 | 对 strong_enabled=false 的提醒调用 snooze / dismiss |
| `ALREADY_DISMISSED` | 强提醒已关闭 | 对 phase=dismissed 的提醒执行 snooze |
| `NO_QUERY_CONDITION` | 未提供查询条件 | query_reminder 的 reminder_id 为空 |
| `INTERNAL_ERROR` | 内部错误 | 存储读写异常、定时器异常等 |

---

## 8. 完整交互时序示例

### 场景 A：创建日程 + 挂载提醒（LLM 组合调用）

```
用户：     "明天上午十点开会，提前 15 分钟提醒我"
LLM 判断：需要日程 + 需要提醒
  ↓
LLM：     create_schedule(event="开会", start_time="2026-07-28T10:00:00+08:00")
日程返回： { schedule_id: 1, schedule: {...}, conflicts: [] }
  ↓
LLM：     add_reminder(target_description="开会",
                      target_time="2026-07-28T10:00:00+08:00",
                      advance_minutes=15, strong_enabled=true)
提醒返回： { reminder_id: 1, reminder: {...} }
  ↓
LLM：     update_schedule(schedule_id=1, reminder_id=1)   // 回写 reminder_id
  ↓
LLM→TTS：  "好的，已添加明天上午十点开会，提前 15 分钟提醒你"
```

### 场景 B：弱提醒触发（无交互）

```
系统：     09:45，弱提醒触发
事件：     reminder_triggered(type=weak, advance_minutes=15)
TTS：      "提醒：你的'项目周会'将在 15 分钟后开始"
弱提醒完成：完成后不再重复
```

### 场景 C：强提醒 + "等会儿"（未达上限）

```
系统：     10:00，强提醒触发
事件：     reminder_triggered(type=strong, retry_count=1)
TTS：      "提醒：你的'项目周会'现在开始了"
用户：     "等会儿再提醒我"
LLM：      snooze_reminder(reminder_id=1)
模块内部： retry_count=1 < 3，推迟 10 分钟 → retry_count=2
模块返回： { snoozed: true, next_trigger_at: "10:10", retry_count: 2 }
LLM→TTS：  "好的，一会儿再提醒你"

系统：     10:10，强提醒再次触发
事件：     reminder_triggered(type=strong, retry_count=2)
TTS：      "提醒：你的'项目周会'已经开始了"
用户：     （无回应）
模块内部： 等 10 分钟 → retry_count=3

系统：     10:20，强提醒第 3 次触发
事件：     reminder_triggered(type=strong, retry_count=3)
TTS：      "提醒：你的'项目周会'已经开始了"
用户：     （无回应）
模块内部： retry_count=3 ≥ max_retries → completed（终止）
```

### 场景 D：强提醒 + 指定推迟时间

```
系统：     10:00，强提醒触发（retry_count=1）
TTS：      "提醒：你的'项目周会'现在开始了"
用户：     "20 分钟后再提醒我"
LLM：      snooze_reminder(reminder_id=1, snooze_minutes=20)
模块内部： retry_count → 2，snoozed until 10:20
模块返回： { snoozed: true, next_trigger_at: "10:20", retry_count: 2 }
LLM→TTS：  "好的，20 分钟后再提醒你"

系统：     10:20，强提醒触发（retry_count=2）
TTS：      "提醒：你的'项目周会'已经开始了"
用户：     "我知道了"
LLM：      dismiss_reminder(reminder_id=1)
模块返回： { dismissed: true }
```

### 场景 E：已达上限时"等会儿"

```
系统：     10:20，强提醒第 3 次触发（retry_count=3）
TTS：      "提醒：你的'项目周会'已经开始了"
用户：     "等会儿再提醒我"
LLM：      snooze_reminder(reminder_id=1)
模块内部： retry_count=3 ≥ 3，延一次，count 不变
模块返回： { snoozed: true, next_trigger_at: "10:30", retry_count: 3 }
LLM→TTS：  "好的，一会儿再提醒你"

系统：     10:30，强提醒最后一次
TTS：      "提醒：你的'项目周会'已经开始了"
用户：     （无回应）
模块内部： 最后一次无回应 → completed
```

### 场景 F：删除日程 + 清理提醒

```
用户：     "取消明天的开会"
LLM：     query_schedule(keyword="开会")
日程返回： { schedules: [{ schedule_id: 1, reminder_id: 1, ... }], total: 1 }
  ↓
LLM：     remove_reminder(reminder_id=1)       // 从 schedule.reminder_id 获取
提醒返回： { reminder_id: 1, removed: true }
  ↓
LLM：     delete_schedule(schedule_id=1)
日程返回： { schedule_id: 1, deleted: true }
LLM→TTS：  "好的，已取消明天的开会"
```

---

## 9. Mock 数据参考

### mock: add_reminder 成功

```json
{
  "reminder_id": 1,
  "reminder": {
    "reminder_id": 1,
    "target_description": "项目周会",
    "target_time": "2026-07-28T10:00:00+08:00",
    "advance_minutes": 15,
    "strong_enabled": true,
    "max_retries": 3,
    "retry_interval_minutes": 10,
    "status": "pending",
    "strong_state": null,
    "created_at": "2026-07-27T14:30:00+08:00",
    "updated_at": "2026-07-27T14:30:00+08:00"
  }
}
```

### mock: snooze_reminder（等会儿）

```json
{
  "reminder_id": 1,
  "next_trigger_at": "2026-07-28T10:10:00+08:00",
  "retry_count": 2,
  "snoozed": true
}
```

### mock: snooze_reminder（指定推迟 20 分钟）

```json
{
  "reminder_id": 1,
  "next_trigger_at": "2026-07-28T10:20:00+08:00",
  "retry_count": 2,
  "snoozed": true
}
```

