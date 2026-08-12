# 定时任务模块需求与设计文档 V4

本文档定义 VoiceLife 的定时任务模块（Timer Task）：接收日程模块注册的 `task_id` 与具体 `trigger_at`，在到点时调用该 task 对应的 `callback`，并支持取消已注册 task，以及多个 task 同时或相近时间到期的执行管理。

日程内容、重复周期、提醒配置以及调用 IM、语音等外部接口的职责全部归日程模块。定时任务模块不理解日程内容、提醒等级、IM、语音、用户、周期规则或 callback 的业务含义。

## 1. 行业调研及成熟方案

1. ESP 平台定时支持 <https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/system/esp_timer.html>
2. FreeRTOS Task Notification / 事件组（ESP-IDF 集成）
3. 设备侧定时能力与第三方库评估 <../docs/architecture/third-party-timing-library-evaluation.md>

本模块的定位是"**何时触发**"，与"触发后做什么"完全解耦：后者由日程模块在 callback 中实现。因此调研聚焦嵌入式定时与任务唤醒机制，不涉及日历展开、提醒语义或消息投递。

## 2. 核心目标

定时任务模块通过三个入口工作：

- `RegisterTask` 注册一个 task（`task_id` + `trigger_at` + `callback`）。
- `CancelTask` 取消一个已注册 task。
- `RunDueTasks` 由 Runner 在到点时推进全部到期 task。

流程：

- 接收日程模块注册的 `task_id` 与具体 `trigger_at`。日程模块已把周期规则计算为具体时间点，本模块不做任何周期解析。
- 在 `trigger_at` 到点时调用该 task 对应的 callback；多个 task 同时或相近到期时，按稳定顺序逐个调用。
- 支持取消已注册 task。
- 使用**单个一次性 esp_timer** 设置最近唤醒时间；到点时 esp_timer callback 只做轻量唤醒，由 Runner 在普通 FreeRTOS task 中处理到期 task。

**本模块不保存日程内容、提醒配置或周期规则，不调用 IM、语音等外部接口，不理解 callback 的业务含义。输出与投递完全由日程模块在 callback 中负责。**

## 3. 核心概念定义

- **`task_id` task 标识**
  - 上游
  - 日程模块分配的不透明标识；本模块只按标识管理，不理解其含义。
  - 术语消歧：本文中的 `task` 指业务调度单位；`Runner` 运行在 FreeRTOS task（系统任务/线程）中，文档中“Runner task”“FreeRTOS task”均指系统线程，与业务 `task` 无关。

- **`trigger_at` 到点时刻**
  - 上游
  - 本模块唯一关注的业务字段。由日程模块把周期规则（RRULE、时区、例外、提醒偏移）计算为具体时间点后传入；本模块不解释它的由来。

- **`callback` 到期回调**
  - 上游
  - 注册 task 时绑定的回调，到点由 Runner 调用。本模块不解析其参数的业务含义，也不决定返回值如何处置；投递、内容组装、后续 task 注册全部由 callback 内部实现。

- **`Runner` 执行单元**
  - 运行在普通 FreeRTOS task 中，负责处理到期 task：取当前时间、组装到期批次、逐个调用 callback、计算下一次唤醒时间。

- **`到期批次`（due batch）**
  - 一次唤醒后，所有 `trigger_at <= now` 的 task 组成的集合，按 `(trigger_at, task_id)` 稳定排序。

- **`唤醒时间`（next wake at）**
  - 当前注册表中最早的 pending `trigger_at`；Runner 用它设置下一次 esp_timer 一次性唤醒。

## 4. 核心业务流程

### 4.1 注册与变更

1. 日程模块创建或更新日程后，把周期规则展开为具体 `trigger_at`，调用 `RegisterTask` 提交 `task_id` 与 `trigger_at`（并绑定 callback）。
2. `task_id` 是单次触发实例的标识：任何 task_id 只允许注册一次，重复注册返回 `duplicate`。
3. 日程变更（含 snooze、dismiss 等提醒运行态操作）由日程模块重新计算 `trigger_at` 后，以“取消旧 task + 注册新 task（新 task_id）”表达更新；本模块不解释变更原因，也不理解“推迟”“关闭”等语义。取消与注册之间的中间态由日程模块负责补偿。

### 4.2 到点推进（重点：多个 task 同时触发）

1. esp_timer 到期，其 callback 默认运行在 ESP-IDF 的 esp_timer task（`ESP_TIMER_TASK` dispatch，**非中断上下文**），只做轻量唤醒/通知 Runner，立即返回；必须短小、非阻塞，不得调用日程模块、IM、语音或任何业务代码。仅当显式选用 `ESP_TIMER_ISR` dispatch 时才运行在中断上下文，限制更严格。
2. Runner 被唤醒后，先消费命令队列中的 `RegisterTask` / `CancelTask` 命令（见 4.3），再取得当前 `now`。
3. **找出全部 `trigger_at <= now` 的 task，而不是只取一个**，组成当前到期批次。
4. 按稳定顺序排序：先按 `trigger_at` 升序，再按 `task_id` 升序。
5. 每个 task 在执行 callback 前，从待执行集合中移除或标记为 `executing`，避免同一次或重复唤醒时重复回调。
6. 默认顺序回调，不要求并发执行；某个 callback 较慢时，后续已到期 task 保留在注册表中，当前 callback 返回后继续处理，**不得丢失**。
7. callback 执行期间可能注册或取消其他 task，变更生效规则见 4.3。
8. 本批次处理完成后，重新查找最早的待执行 task，将 esp_timer 设置为该 `trigger_at` 的一次性唤醒；无待执行 task 则不设置定时器。

### 4.3 变更生效规则（并发与批次语义）

**注册表写者唯一**：task 注册表只由 Runner task 读写。

- **外部任务**（MCP、IM 等可能调用 `RegisterTask` / `CancelTask` 的上下文）不能直接改注册表：公开接口将命令异步投递到 Runner 的命令队列（FreeRTOS Queue），立即返回 `accepted`，由 Runner 独占消费并应用。Runner 每轮在组装到期批次**之前**消费命令，故外部变更反映到本轮或下一轮的批次组装。最终应用结果通过命令结果回调或事件通知调用方。
- **callback 内部**（已在 Runner task 中）可以直接调用 `RegisterTask` / `CancelTask`，立即作用于注册表。
- 命令与到期 task 都在 Runner 内顺序处理；命令积压不会丢失，但外部取消或注册要等 Runner 消费命令后才生效。

**当前到期批次在开始执行时确定**。

- 批次确定后新注册的 task 不加入当前批次，按 `trigger_at` 参与下一次调度。
- 批次内尚未执行的 task，轮到它时校验注册表中是否仍存在：
  - task 已不存在（被取消；更新 = 取消旧 id + 注册新 id，故旧 id 同样表现为不存在）→ 跳过执行。
  - 仍存在 → 正常执行。
- 由于 task_id 永不复用，批次项与注册表不会出现“同 id 双代际”，无需版本号区分。
- 上述规则保证：callback 与外部命令引起的变更不破坏当前批次的执行，也不会丢失任何 task。

### 4.4 取消

`CancelTask` 将 task 标记为 `cancelled` 并从注册表移除；若该 task 已在当前批次中且尚未执行，执行前跳过；正在执行的 task 不受影响（其 callback 已开始）。

### 4.5 硬件定时与唤醒

1. **只使用一个一次性 esp_timer**，不为每个 task 创建一个定时器。每次设置为当前注册表中最早的 pending `trigger_at`。
2. esp_timer callback 默认运行在 ESP-IDF 的 esp_timer task（`ESP_TIMER_TASK` dispatch，非中断上下文），仍必须短小、非阻塞，只允许轻量操作（如 Task Notification / 事件组通知 Runner），禁止调用日程模块、IM、语音或其他业务代码；仅选用 `ESP_TIMER_ISR` dispatch 时才进入中断上下文。
3. Runner 是普通 FreeRTOS task，负责批次处理、callback 调用与下一次唤醒时间计算；回调中较重的业务逻辑（内容组装、网络、投递）在 Runner task 中执行。
4. `trigger_at` 是墙上时钟的绝对时间；本模块通过 `Clock` 取得当前墙上时间，计算 `max(0, trigger_at - now)` 的相对微秒延时后调用 `esp_timer_start_once()`。`Clock` 由 Runtime 注入，本模块负责时间换算和重新设置定时器。
5. 无待执行 task 时不设置定时器，设备可进入低功耗。

## 5. 模块接口

### 5.1 接口总览

| 接口 | 调用方 | 说明 |
| --- | --- | --- |
| `RegisterTask` | 日程模块 | 异步提交注册 task（`task_id` + `trigger_at` + callback） |
| `CancelTask` | 日程模块 | 异步提交取消已注册 task |
| `RunDueTasks` | Runner（内部） | 推进到期 task 与下一次唤醒调度；不是面向业务调用方的查询 API |

### 5.2 接口参数

#### 5.2.1 `RegisterTask`

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| `task_id` | string | 是 | 非空，单次唯一 | task 标识；任何 task_id 都不可复用 |
| `trigger_at` | datetime | 是 | ISO 8601 | 到点时刻，由日程模块计算 |
| `callback` | function | 是 | 注册时绑定 | 到点回调；运行时绑定，不持久化 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `result` | string | `accepted` | 命令已进入 Runner 队列；最终结果通过命令结果回调或事件通知 |

约束：`trigger_at` 必须是绝对时间点，本模块不接收周期表达式、偏移量或提醒等级。`task_id` 为单次触发实例标识，任何 task_id 都不可复用；更新必须使用新的 task_id，并先取消旧 task。最终注册结果由 Runner 通过命令结果回调或事件通知 `registered` / `duplicate`。

#### 5.2.2 `CancelTask`

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| `task_id` | string | 是 | 非空 | 待取消 task 标识 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `result` | string | `accepted` | 命令已进入 Runner 队列；最终结果通过命令结果回调或事件通知 |

#### 5.2.3 `RunDueTasks`

**请求参数**

| 参数名 | 类型 | 必填 | 约束 | 说明 |
| --- | --- | --- | --- | --- |
| `now` | datetime | 是 | ISO 8601 | 本轮到期边界，Runner 唤醒时取得 |

**返回参数**

| 参数名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `processed_count` | integer | >= 0 | 本轮已调用 callback 的 task 数 |
| `skipped_count` | integer | >= 0 | 本轮批次中被取消而跳过的 task 数 |
| `next_wake_at` | datetime | 可空 | Runner 应设置的最近唤醒时间 |

### 5.3 依赖与 Port

本模块只依赖：

- `esp_timer`：一次性定时唤醒。
- FreeRTOS Task Notification / 事件组：esp_timer callback 唤醒 Runner。
- `Clock`：提供当前墙上时间；本模块将绝对 `trigger_at` 换算为 `esp_timer` 所需的相对微秒延时。

**没有输出 Port**。到点后的业务动作（内容组装、IM 投递、语音播报、注册下一个周期 task）全部由 callback 内部完成，本模块不定义也不感知这些动作。

### 5.4 状态约定

- `task.status`
  - `pending`：已注册，等待触发。
  - `executing`：已从待执行集合取出，正在调用 callback（防重复回调）。
  - `completed`：callback 已返回，终态。
  - `cancelled`：已取消，终态。
  - 允许流转：`pending -> executing / cancelled`，`executing -> completed`。
  - 批次中尚未执行即被取消的 task：`pending -> cancelled`，跳过 callback。

## 6. 主要数据模型

### 6.1 `task`

| 字段名 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `task_id` | string | 主键，唯一，非空 | task 标识 |
| `trigger_at` | datetime | 非空 | 到点时刻 |
| `status` | string | 非空，枚举 | `pending` / `executing` / `completed` / `cancelled` |
| `created_at` | datetime | 非空 | 创建时间 |
| `updated_at` | datetime | 非空 | 最后更新时间 |

说明：

- `callback` 为运行时绑定（注册时注入），**不持久化**；task 表只存调度所需字段。
- 注册表要求支持：按 `trigger_at` 快速取最早 task（设置下一次唤醒）、取全部 `trigger_at <= now` 的 task（组装批次）、按 `task_id` 精确查找（取消、校验存在性）。MVP 规模下用按 `trigger_at` 排序的有序集合 + `task_id` 索引即可；实现可用堆或排序数组。
- 持久化策略：设备在良好条件下持续运行，暂不考虑断电、重启场景，注册表可驻留内存。若后续需要跨重启保留，由日程模块在启动时重新注册全部 task（本模块不负责恢复业务状态）。

### 6.2 到期批次（运行时，不持久化）

一次唤醒确定的到期 task 快照，按 `(trigger_at, task_id)` 排序；执行中的 task 从注册表待执行集合移除，防止重复唤醒时重复回调；执行前校验 task 是否仍存在（见 4.3）。

## 7. 与日程模块的边界（callback 契约）

| 职责 | 归属 |
| --- | --- |
| 日程内容、周期规则、提醒配置 | 日程模块 |
| 周期展开为具体 `trigger_at` | 日程模块 |
| 到点后的投递（IM、语音）与内容组装 | 日程模块（callback 内实现） |
| 注册、取消 task | 本模块（由日程模块异步提交） |
| 到期调用 callback、批量执行、下一次唤醒调度 | 本模块 |

callback 建议签名：`on_task_due(task_id, trigger_at)`。日程模块在 callback 内部决定：投递什么内容、是否注册下一个周期 task、是否需要更新或取消其他 task。callback 的执行时间直接影响同批次后续 task 的处理，但不会导致它们丢失（4.2 第 6 条）。

## 8. 运行假设与限制

- 设备默认在良好条件下持续运行，暂不考虑断电、重启、断网等极端场景；本模块不提供跨重启恢复和投递重试。
- 本模块不保证回调之间并发，默认顺序执行；并发是日程模块 callback 内部的取舍，不在本模块范围。
- `trigger_at` 精度取决于 esp_timer 与系统时钟；到点即处理，不承诺毫秒级精确。
