# 硬件语音助手的 IM 辅助通道架构草案

> 状态：一期架构草案。本文只描述“硬件语音主交互 + 企业微信单聊辅助”的主干，不以现有 live demo 或已有代码为依据。

## 一、需求与边界

产品运行在硬件上，用户主要通过语音完成交互。IM 只是语音播报的辅助通道：提供可回看、可点击的卡片，不是第二个聊天入口。

一期支持：

- 企业微信自建应用；
- 一个硬件用户绑定一个企业微信成员，使用一对一会话；
- 业务事件主动发送模板卡片；
- 卡片按钮触发“知道了”“10 分钟后提醒”等受控操作；
- 用户查询今日日程时，语音播报摘要，同时发送日程卡片。

一期不支持群聊、文本指令、多 IM 平台路由、聊天记录同步、已读状态和附件。

## 二、核心决策与风险

| 决策 | 结论 |
| --- | --- |
| 主入口 | 硬件语音；IM 仅辅助。 |
| 一期平台 | 企业微信自建应用，便于公司内部用户联调。 |
| 状态归属 | 业务服务是提醒、日程和操作结果的唯一事实来源。 |
| 双通道关系 | 同一业务事件并行驱动语音和 IM；任何一侧失败不阻塞另一侧或业务状态。 |
| 上行方式 | 只接受模板卡片按钮回调，不解析 IM 自由文本。 |

一期必须处理三类风险：

1. 业务状态已提交，但语音或 IM 尚未收到事件；
2. 用户点击后平台停止重试、服务却在真正处理前中断；
3. 同一个按钮被重复点击或被平台重复回调。

对应原则是：业务事件可靠记录；回调先持久化再 ACK；同一按钮对应稳定的幂等操作 ID。

## 三、模块架构

业务服务在修改业务状态时，可靠地记录一条业务事件。提交后，内部任务将同一事件并行交给硬件语音通道和 IM 辅助模块。这个内部任务是业务服务的实现细节，不单独展开为架构模块。

```text
                              ┌───────────────────┐
                              │     业务服务       │
                              │  状态、规则与查询   │
                              └───┬───────────▲───┘
                                  │业务事件 /   │受控操作
                                  │查询结果     │
                    ┌─────────────┴──────────┐ │
                    ▼                        ▼ │
         ┌───────────────────┐      ┌───────────────────┐
         │    硬件语音通道    │      │    IM 辅助模块      │─────► 企业微信应用单聊
         │      语音播报      │      │ 卡片、回调与投递     │              │ 卡片回调
         └───────────────────┘      └─────────▲─────────┘              │
                                              └────────────────────────┘
```

| 模块 | 职责 | 不负责 |
| --- | --- | --- |
| 业务服务 | 保存业务状态、执行确认/推迟规则、产生业务事件与查询快照 | 企业微信验签、卡片渲染和 API 调用。 |
| 硬件语音通道 | 消费业务事件并播报；接收硬件语音输入 | IM 投递与卡片回调。 |
| IM 辅助模块 | 把业务事件转成卡片；可靠投递；接收、持久化并处理卡片回调 | 决定提醒或日程的业务规则。 |
| 企业微信应用 | 展示一对一卡片，并传回按钮操作 | 保存业务状态或判断操作有效性。 |

IM 辅助模块内部可以有投递和入站处理任务，但它们是模块内部实现，不在主架构图中单列。

## 四、平台无关的数据模型

以下是一期必须落地的逻辑模型。它们可以实现为四张表，也可以在同一数据库中按服务边界拆分；本文固定字段语义与约束，不限制具体 ORM 或存储引擎。

### 1. 用户绑定 `im_bindings`

| 字段 | 说明 |
| --- | --- |
| `id` | 内部绑定 ID。 |
| `user_id` | 硬件产品的内部用户 ID。 |
| `platform` | 一期固定为 `wecom`，保留字段用于后续渠道审计。 |
| `external_user_id` | 企业微信中该成员的应用作用域身份。 |
| `external_conversation_id` | 企业微信发送一对一消息所需的接收者标识。 |
| `status` | `active`、`unbound`、`unreachable`。 |
| `bound_at` / `unbound_at` | 绑定审计时间。 |

约束：一期 `user_id` 和 `(platform, external_user_id)` 都唯一。`user_id` 是产品内部身份；`external_user_id` 是企业微信成员身份，不能用昵称或展示名代替。

### 2. 业务事件 `business_events`

业务服务拥有此模型。业务状态变更时，它与状态在同一事务中可靠写入；查询结果也作为带展示快照的事件写入。

| 字段 | 说明 |
| --- | --- |
| `id` | 业务事件 ID，也是语音和 IM 消费的幂等来源。 |
| `event_type` | `reminder_due`、`action_completed`、`daily_schedule_ready`。 |
| `user_id` | 事件归属用户。 |
| `aggregate_type` / `aggregate_id` | 关联的提醒、日程或查询对象。 |
| `payload` | 不可变的处理/展示快照；日程事件包含用户时区下的列表快照。 |
| `operation_id` | 发起该业务变更的稳定操作 ID；查询使用请求 ID。 |
| `delivery_status` | `pending`、`dispatched`、`retryable_failed`。 |
| `created_at` / `dispatched_at` | 审计时间。 |

约束：同一 `event_type + operation_id` 只生成一条事件。这里的可靠记录可用事务外盒实现，但不要求 IM 模块拥有该表。

### 3. IM 卡片 `im_cards`

一条卡片是一个业务事件在某个用户 IM 中的展示和投递工作项；它合并了“回执”和“待发送卡片”两个一期不需要分开的概念。

| 字段 | 说明 |
| --- | --- |
| `id` | 内部卡片 ID。 |
| `business_event_id` | 唯一来源业务事件。 |
| `binding_id` | 目标用户绑定。 |
| `kind` | `reminder_due`、`action_result`、`daily_schedule`。 |
| `title` / `body` / `data` | 平台无关的卡片文案和结构化展示快照。 |
| `platform_task_id` | 交互型卡片在平台侧的稳定标识；企业微信中为 `task_id`。 |
| `status` | `pending`、`sending`、`accepted`、`failed`、`dead_letter`。 |
| `attempt_count` / `next_attempt_at` | 投递重试控制。 |
| `external_message_id` | 平台接受发送请求后返回的消息标识，可为空。 |
| `created_at` / `expires_at` | 生命周期。 |

约束：`(business_event_id, binding_id, kind)` 唯一；`platform_task_id` 在同一平台内唯一。重试不得重新生成 `platform_task_id`。

### 4. 卡片操作 `im_card_actions`

每个可点击按钮一条记录，同时承担回调收件和操作授权的职责；一期无需将两者拆成独立模型。

| 字段 | 说明 |
| --- | --- |
| `id` | 内部操作 ID。 |
| `card_id` | 所属 IM 卡片。 |
| `action_type` | `acknowledge` 或 `snooze_10_minutes`。 |
| `target_type` / `target_id` | 可操作的内部业务对象。 |
| `actor_user_id` | 唯一允许点击的内部用户。 |
| `action_key_hash` | 按钮携带标识的哈希；企业微信中对应 `EventKey`。 |
| `operation_id` | 服务端生成的稳定业务幂等键。 |
| `callback_dedupe_key` | 回调持久化后的去重键；由平台任务、按钮、成员和时间等真实回调字段生成。 |
| `status` | `pending`、`received`、`processing`、`executed`、`rejected`、`retryable_failed`、`expired`。 |
| `received_at` / `processed_at` / `expires_at` | 回调、处理和有效期审计时间。 |

约束：`action_key_hash` 唯一；同一回调去重键只接受一次。处理时必须原子地取得执行权，恢复处理始终复用 `operation_id`。明文按钮标识不写入数据库或日志。

企业微信适配时，`platform_task_id` 对应回调 `TaskId`，`action_key_hash` 对应 `EventKey` 的哈希；回调成员必须与卡片 `binding_id` 对应的用户一致。

## 五、接口契约

### 1. IM 辅助模块

```ts
interface ImAssistService {
  // 消费已可靠记录的业务事件，创建卡片和需要的按钮；重复调用安全。
  projectBusinessEvent(input: {
    businessEventId: string;
  }): Promise<{ cardId?: string; duplicate: boolean }>;

  // 在返回平台 ACK 前持久化回调；重复回调返回既有操作。
  recordCardAction(input: {
    platformTaskId: string;
    actionKey: string;
    externalUserId: string;
    callbackTimestamp: number;
  }): Promise<{ actionId: string; duplicate: boolean }>;

  // 异步处理已持久化的操作，并以稳定 operationId 调用业务服务。
  processCardAction(input: {
    actionId: string;
  }): Promise<{
    outcome: "executed" | "duplicate" | "rejected" | "retryable_failed";
    resultBusinessEventId?: string;
  }>;
}
```

`projectBusinessEvent` 只根据业务事件创建 IM 卡片；操作成功后，业务服务产生结果事件，再由这个入口创建结果卡片。不要在回调处理里直接额外创建一张结果卡片。

```ts
interface BusinessActionService {
  execute(input: {
    operationId: string;
    userId: string;
    actionType: "acknowledge" | "snooze_10_minutes";
    targetType: string;
    targetId: string;
  }): Promise<{ resultBusinessEventId: string; duplicate: boolean }>;
}
```

IM 模块只能通过此幂等接口请求业务服务执行操作；业务服务负责校验当前业务状态并生成结果事件。

### 2. 企业微信适配器

```ts
interface ImPlatformGateway {
  sendCard(input: {
    externalConversationId: string;
    platformTaskId: string;
    title: string;
    body: string;
    data?: Record<string, unknown>;
    buttons: Array<{ label: string; actionKey: string }>;
  }): Promise<{ externalMessageId?: string }>;

  verifyAndParseCallback(request: HttpRequest): Promise<{
    platformTaskId: string;
    actionKey: string;
    externalUserId: string;
    callbackTimestamp: number;
  }>;
}
```

企业微信实现中，`platformTaskId` 为 `task_id`，`actionKey` 为 `EventKey`。适配器负责验签、解密、字段转换和平台 API 调用；它不识别“提醒”“日程”等业务概念，也不执行确认或推迟。

### 3. 回调入口

```http
POST /im/callback
```

处理顺序固定为：验签和解密 → 校验卡片与成员对应关系 → 持久化回调 → 返回平台 ACK → 异步执行业务操作。

持久化失败时不能返回成功 ACK，让企业微信重试。已持久化的重复回调可以直接 ACK；后台会继续处理未完成的操作。

## 六、主干流程

### 1. 提醒到达

1. 业务服务完成提醒到期状态变更，并可靠记录“提醒到达”事件。
2. 同一事件并行交给硬件语音通道和 IM 辅助模块。
3. 硬件开始播报；IM 模块创建提醒卡片和“知道了”“10 分钟后提醒”按钮，并异步投递到企业微信。
4. IM 投递失败只重试 IM，不回滚提醒状态，也不影响语音。

### 2. 用户点击 IM 卡片

1. 企业微信回调到达，适配器验签、解密并解析 `TaskId`、`EventKey` 和成员。
2. IM 模块确认该成员、卡片和按钮匹配，持久化该操作后返回 ACK。
3. 后台以该按钮固定的操作 ID 调用业务服务执行确认或推迟；重复点击只得到同一个业务结果。
4. 业务服务产生“操作完成”事件；它再次并行驱动语音播报和 IM 结果卡片。

### 3. 用户查询今日日程

1. 用户通过硬件发起查询；业务服务按用户时区生成一次日程展示快照，并可靠记录查询结果事件。
2. 硬件播报摘要；IM 模块使用同一份快照发送“今日日程”卡片。
3. 一期日程卡片只用于查看，不提供 IM 上行按钮。

## 七、可靠性实现约束

以下内容是实现要求，不是新增业务模块：

- 业务状态变更与业务事件必须一起成功或一起失败；常见实现是事务外盒（outbox）。
- 卡片回调在 ACK 前必须落入可恢复的收件箱（inbox）或等价的事务队列。
- 每个按钮有一次性、短期有效的服务端标识；处理时必须原子认领，并始终复用同一个业务操作 ID。
- 业务事件、卡片和操作处理都必须支持重复投递；结果卡片只由“操作完成”业务事件生成一次。
- 企业微信 `task_id` 在重试时保持不变，避免回调无法关联历史卡片。

## 八、一期验收与演进

上线前以真实企业微信自建应用完成闭环验证：

1. 测试成员在应用可见范围内，能收到一对一模板卡片；
2. “知道了”“10 分钟后提醒”按钮能回调并识别正确成员；
3. 重放同一回调、并发点击同一按钮、服务在 ACK 后中断，均只产生一次业务变更；
4. 业务状态提交后、语音和 IM 分发前中断，重启后仍会补发两个通道；
5. 查询今日日程时，语音和 IM 使用同一份快照。

后续如果接入微信，作为独立渠道新增适配器、身份绑定和卡片/回调协议；不复用企业微信的凭证、回调格式或 `task_id` 规则。多渠道路由、群聊和文本命令都必须在新的架构变更中单独评估。
