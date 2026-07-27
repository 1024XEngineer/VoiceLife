# 硬件语音助手的 IM 辅助通道架构草案

> 状态：架构草案。本文只描述“硬件语音主交互 + 一个 IM 单聊辅助通道”的主干，不以现有 live demo 或已有代码为依据。

## 一、需求梳理与边界

### 1. 产品形态

产品运行在硬件上，用户主要通过语音完成交互。IM 不是第二个对话入口，而是语音播报的辅助通道：在语音提醒、操作结果等业务事件发生时，提供可回看且可点击的消息卡片。

第一阶段只支持：

- 企业微信自建应用；
- 用户与产品机器人的一对一会话；
- 业务事件向应用可见范围内的成员主动发送模板卡片；
- 用户点击模板卡片按钮触发受控操作，例如“知道了”“10 分钟后提醒”。

第一阶段明确不支持：

- 多平台同时投递或渠道切换；
- 群聊、@ 机器人、群内消息读取；
- 用户发送文本后由 IM 解析开放式指令；
- 同步 IM 聊天记录、已读状态或附件；
- 以 IM 替代硬件语音的主交互。

### 2. 主干场景

1. 到期事件发生，开始语音播报；业务系统同时向绑定用户的 IM 单聊发送提醒卡片。
2. 用户语音交互处理该提醒，业务状态变更；IM 侧发送或更新结果卡片。
3. 用户点击 IM 卡片的“知道了”或“10 分钟后提醒”；平台把卡片回调传给业务系统，业务系统完成相应操作；硬件与 IM 都展示最终结果。
4. 用户查询今日日程，语音播报日程同时发送日程安排卡片。

语音和 IM 是两个并行出口，任何一侧的发送或展示失败都不能阻止业务状态变更，也不能阻塞另一侧。

### 3. 核心设计决策

| 决策 | 结论 | 原因 |
| --- | --- | --- |
| 交互入口 | 硬件语音为主，IM 仅作辅助 | 保持产品形态一致，避免演化成双入口聊天助手。 |
| IM 数量 | 一期一个平台 | 优先验证完整闭环，避免多适配器和多渠道状态一致性。 |
| 会话范围 | 仅一对一 | 不引入群权限、@ 规则与多用户归属。 |
| 上行方式 | 仅卡片按钮回调 | 操作对象明确，避免文本歧义与误操作。 |
| 业务状态 | 业务系统为唯一事实来源 | 平台消息、卡片显示和回调均可能延迟、重复或失败。 |
| 投递策略 | 事务外盒（outbox）异步投递 | 业务变更不能依赖第三方 IM API 的可用性。 |
| 一期 IM 平台 | 企业微信自建应用 | 公司已在使用，首批用户与联调环境可直接纳入应用可见范围。 |

## 二、风险分析

| 风险 | 影响 | 架构应对 | 上线前验证 |
| --- | --- | --- | --- |
| 企业微信成员不在应用可见范围 | 无法发送卡片或接收回调 | 将首批体验用户纳入自建应用可见范围，并在绑定时校验成员身份 | 用真实成员完成一次发送与回调闭环。 |
| 回调重复、超时或乱序 | 重复关闭、重复推迟 | 卡片操作采用一次性授权和业务幂等键；回调收件箱去重 | 人为重放同一平台事件。 |
| 回调 ACK 后、业务处理前进程中断 | 平台停止重试，用户操作丢失 | ACK 前持久化 inbox；入站处理工作者通过认领与超时恢复继续处理 | 事件落库后中断进程，重启后确认只执行一次。 |
| 平台接受发送请求但用户未看到 | 误判通知成功 | 区分“平台已接受”和“用户已读”；一期只承诺前者 | 模拟网络超时与平台 API 超时。 |
| IM 身份无法可靠对应硬件用户 | 误向他人发送或执行操作 | 采用显式绑定，不以昵称或外部 ID 直接认领身份 | 解绑、换号、重复绑定测试。 |
| 硬件离线或 IM 不可用 | 双通道状态不一致 | 业务状态独立持久化；两个出口分别重试或降级 | 分别断开硬件、断开 IM 进行演练。 |
| 卡片旧按钮仍可点击 | 过期操作影响当前状态 | 每个按钮绑定短期、一次性的服务器签发 token | 对过期、已消费 token 发起回调。 |

## 三、模块架构

业务服务完成状态变更后，在同一事务写入并发布领域事件；查询不改变业务状态，但也产出一条业务结果事件。业务服务内部的事件分发机制将同一事件并行交给硬件语音通道和 IM 辅助模块；它不是总架构中的独立模块。

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
         │    硬件语音通道    │       │    IM 辅助模块     │─────► 企业微信应用单聊
         │      语音播报      │      │ 卡片、授权与可靠投递  │              │
         └───────────────────┘      └─────────▲─────────┘              │ 卡片回调
                                              └────────────────────────┘
```

IM 辅助模块内部负责回调验签、inbox 持久化、操作授权、outbox 投递、重试和企业微信适配。平台 ACK 只能在 inbox 已持久化后返回；模块内部的入站处理任务负责恢复已落库但尚未处理完成的事件。两条 IM 路径都不经过硬件语音通道。

### 模块职责

| 模块 | 职责 | 不负责 |
| --- | --- | --- |
| 业务服务 | 保存业务状态、执行“确认/推迟”等规则、产生领域事件或查询结果 | 平台验签、卡片渲染、调用 IM API。 |
| 硬件语音通道 | 接收业务事件或查询结果并播报；接收硬件语音输入 | IM 的投递和回调处理。 |
| IM 辅助模块 | 将业务事件转成卡片回执；处理卡片回调；保障 inbox/outbox 的持久化、重试与恢复；调用企业微信能力 | 决定业务规则或直接改写业务状态。 |
| 企业微信应用 | 展示一对一模板卡片，并把按钮操作回调给 IM 辅助模块 | 保存产品业务状态或决定操作是否有效。 |

## 四、关键数据模型

模型只保留单平台、一对一、卡片按钮所需的对象；不建立多渠道路由、群会话或文本消息模型。

### 1. `im_bindings`：硬件用户与 IM 单聊绑定

| 字段 | 说明 |
| --- | --- |
| `id` | 内部绑定 ID。 |
| `user_id` | 硬件产品的内部用户 ID。 |
| `platform` | 一期固定为 `wecom`；保留字段用于审计和后续微信渠道扩展。 |
| `external_user_id` | 平台中该用户的应用作用域身份。 |
| `external_conversation_id` | 与机器人一对一会话的 ID 或平台所需接收者 ID。 |
| `status` | `active`、`unbound`、`unreachable`。 |
| `bound_at` / `unbound_at` | 绑定审计时间。 |

约束：一期要求 `user_id` 唯一且 `external_user_id` 唯一；一个硬件用户只绑定一个 IM 单聊。

### 2. `im_receipts`：可投递的 IM 回执

表示业务事件的 IM 展示副本，不是业务状态本身。

| 字段 | 说明 |
| --- | --- |
| `id` | 回执 ID。 |
| `user_id` | 目标用户。 |
| `kind` | `reminder_due`、`action_result`、`daily_schedule`。 |
| `origin_type` / `origin_id` | 回执来源：领域事件使用 `event`，用户查询使用 `request`；两者共同用于去重。 |
| `title` / `body` | 与平台无关的卡片内容。 |
| `data` | 卡片所需的结构化展示字段。 |
| `created_at` / `expires_at` | 生命周期。 |

约束：`(user_id, kind, origin_type, origin_id)` 唯一。

`daily_schedule` 的 `data` 是查询当刻的展示快照，不是 IM 投递时再查询的日程引用：

```json
{
  "date": "2026-07-27",
  "timeZone": "Asia/Shanghai",
  "generatedAt": "2026-07-27T09:00:00+08:00",
  "totalCount": 3,
  "items": [
    {
      "scheduleId": "schedule_…",
      "title": "产品评审",
      "displayTimeRange": "10:00–11:00",
      "location": "会议室 A"
    }
  ]
}
```

语音播报和 IM 卡片都使用同一份查询结果快照，避免两条通道因日程随后变化而展示不一致。

### 3. `im_action_grants`：卡片按钮授权

每个按钮对应一条服务端签发的授权；平台回调必须携带它，不能直接带业务对象 ID 执行操作。

| 字段 | 说明 |
| --- | --- |
| `id` | 授权 ID。 |
| `receipt_id` | 所属 IM 回执。 |
| `action_type` | 一期为 `acknowledge` 或 `snooze_10_minutes`。 |
| `target_type` / `target_id` | 可操作的内部业务对象。 |
| `actor_user_id` | 唯一允许操作者。 |
| `token_hash` | 卡片携带的一次性 token 的哈希。 |
| `expires_at` / `consumed_at` | 有效期与首次消费时间。 |
| `result_receipt_id` | 执行后产生的结果回执。 |

约束：`token_hash` 唯一。明文 token 只进入卡片负载，绝不记录在日志或数据库明文字段。

### 4. `im_outbox`：待发送卡片

| 字段 | 说明 |
| --- | --- |
| `id` | 工作项 ID。 |
| `receipt_id` | 待投递的回执。 |
| `binding_id` | 目的用户绑定。 |
| `render_version` | 卡片模板与字段解释版本。 |
| `dedupe_key` | `receipt_id + binding_id + render_version`；唯一。 |
| `status` | `pending`、`sending`、`accepted`、`failed`、`dead_letter`。 |
| `attempt_count` / `next_attempt_at` | 重试控制。 |
| `external_message_id` | 平台接受后返回的消息 ID。 |
| `last_error` | 脱敏后的失败原因。 |

### 5. `im_inbound_events`：平台卡片回调收件箱

| 字段 | 说明 |
| --- | --- |
| `id` | 内部入站事件 ID。 |
| `platform_event_id` | 平台事件 ID；用于去重。 |
| `external_message_id` | 卡片所属平台消息 ID。 |
| `binding_id` | 标准化后命中的 IM 绑定。 |
| `action_token_hash` | 卡片操作 token 的哈希；用于查找授权，绝不保存明文 token。 |
| `status` | `received`、`processing`、`processed`、`rejected`、`retryable_failed`、`dead_letter`。 |
| `attempt_count` / `next_attempt_at` / `claimed_at` | 认领、重试与崩溃恢复控制。 |
| `received_at` / `processed_at` / `error_code` | 审计与排障。 |

约束：`platform_event_id` 唯一。平台没有事件 ID 时，由适配器以平台消息 ID、操作值和时间窗口生成去重指纹。

## 五、接口契约

### 1. IM 辅助通道服务

```ts
interface ImAssistService {
  createReceipt(input: {
    userId: string;
    kind: "reminder_due" | "action_result" | "daily_schedule";
    origin: { type: "event" | "request"; id: string };
    title: string;
    body: string;
    data?: Record<string, unknown>;
    actions?: Array<"acknowledge" | "snooze_10_minutes">;
  }): Promise<{ receiptId: string }>;

  recordCardAction(input: {
    platformEventId: string;
    externalUserId: string;
    externalMessageId?: string;
    actionToken: string;
  }): Promise<{
    inboundEventId: string;
    duplicate: boolean;
  }>;

  processInboundEvent(input: {
    inboundEventId: string;
  }): Promise<{
    outcome: "executed" | "duplicate" | "rejected" | "retryable_failed";
    resultReceiptId?: string;
  }>;
}
```

`createReceipt` 必须在同一事务中写入 `im_receipts`、所需 `im_action_grants` 与 `im_outbox`，但不直接调用平台 API。

`recordCardAction` 必须在返回成功前将 `im_inbound_events` 写入数据库，以 `platform_event_id` 去重，并仅保存 `action_token_hash`。只有持久化成功（包括识别到既有重复事件）后，回调处理器才能返回平台 ACK。

`processInboundEvent` 由 IM 入站处理工作者执行：原子认领 `received` 或到期的 `retryable_failed` 事件；查找并验证绑定、token 哈希、操作者、有效期和消费状态；调用业务服务；创建结果回执。进程崩溃后，`claimed_at` 超时的 `processing` 事件必须重新变为可认领。业务服务仍要使用独立幂等键，不能只依赖回调去重。

### 2. 企业微信适配器

```ts
interface WeComGateway {
  verifyCallback(request: HttpRequest): Promise<VerifiedCallback>;

  parseTemplateCardAction(callback: VerifiedCallback): Promise<{
    platformEventId: string;
    externalUserId: string;
    externalMessageId?: string;
    actionToken: string;
    responseCode?: string;
  }>;

  sendTemplateCard(input: {
    externalConversationId: string;
    title: string;
    body: string;
    buttons: Array<{ label: string; actionToken: string }>;
  }): Promise<{ externalMessageId: string; responseCode?: string }>;

  updateClickedCard?(input: {
    responseCode: string;
    replacementText: string;
  }): Promise<void>;
}
```

适配器只认识企业微信请求和通用卡片模型。它不认识“提醒”“日程”等具体业务对象，也不执行业务状态变更。`updateClickedCard` 是展示优化；更新凭证的时效和次数限制不能影响业务操作结果。

### 3. 回调入口

```http
POST /im/callback
```

处理顺序：

1. 验签、解密和时间戳校验；
2. 解析卡片操作，调用 `ImAssistService.recordCardAction`，将事件以唯一键持久化为 `received`；
3. 持久化成功后返回该平台要求的 ACK；持久化失败时不返回成功 ACK，让平台重试；
4. IM 入站处理工作者认领该事件，并调用 `ImAssistService.processInboundEvent`；
5. 结果由新的 IM 回执和硬件语音通道分别发布。

对于重复回调，只要对应事件已经持久化即可返回 ACK：已处理事件直接结束，`received`、超时的 `processing` 或可重试失败事件由入站处理工作者继续或恢复处理。企业微信回调使用公网 HTTPS 地址；该入口是唯一对外暴露的 IM 回调地址。回调验签和解密由企业微信适配器完成，后续收件箱和服务接口不依赖企业微信的原始请求格式。

## 六、关键流程

### 1. 提醒到达

1. 业务服务完成到期状态变更并产生领域事件。
2. 领域事件分发器将同一事件并行交给硬件语音通道和 IM 辅助通道服务；两者互不等待。
3. 硬件语音通道开始播报；IM 辅助通道服务创建 `im_receipt`，签发“知道了”“10 分钟后提醒”的 `im_action_grants`，并在同一事务写入 `im_outbox`。
4. IM 投递工作者调用企业微信适配器发送模板卡片。
5. 成功只记为平台已接受；失败按退避策略重试，不回滚已发生的业务状态。

### 2. 用户在 IM 点击“知道了”

1. 企业微信模板卡片回调到达，适配器完成验签、解密和标准化。
2. 回调处理器将事件持久化为 `im_inbound_events.received`，按平台事件 ID 去重；成功落库后才返回 ACK。
3. IM 入站处理工作者认领事件，校验 token 未过期、未消费，且回调用户等于 `actor_user_id`。
4. 业务服务执行确认操作，生成新的业务事件；进程中断或可重试失败时，事件保留并由工作者恢复处理。
5. 领域事件分发器将结果并行发送给硬件语音通道和 IM 辅助通道服务；平台是否支持更新原卡片只是展示优化，不影响业务结果。

### 3. 硬件侧已经处理

硬件侧执行同一业务操作时，也必须使用业务幂等键。后续到达的卡片回调会被识别为已处理，返回稳定结果，而不是再次变更。原卡片可在平台支持时更新为“已处理”；不支持时发送简短结果卡片或保留原卡片。

### 4. 用户查询今日日程

1. 用户通过硬件发起“今日日程”查询；业务服务以用户时区生成一次完整的日程展示快照，并发布查询结果事件。
2. 分发器将同一快照并行交给硬件语音通道和 IM 辅助通道服务；硬件播报摘要，IM 服务以同一个查询请求 ID 创建 `daily_schedule` 回执和 `im_outbox` 工作项。
3. 投递工作者将快照渲染为“今日日程”卡片并发送到用户的一对一会话。该卡片一期不创建 `im_action_grants`，不接收 IM 上行操作。
4. 后续日程发生变化时，不改写这张历史查询卡片；用户下一次查询会生成新的回执和新卡片。

## 七、实现顺序与验收门槛

在写正式骨架代码前，先以企业微信真实自建应用完成以下四项联调：

1. 将测试成员加入应用可见范围，建立应用与成员的一对一消息通道；
2. 从服务端主动发送带“知道了”“10 分钟后提醒”按钮的模板卡片；
3. 用户点击后，服务端收到可验签、可解密的回调且能识别成员；
4. 服务端在业务状态已经变化时，能安全地显示“已处理”，而不是重复执行。
5. 在回调写入 inbox 后、执行业务操作前中断服务；重启后确认事件恢复处理且业务只变更一次。

通过后按以下顺序实现：

1. 定义模块接口与以上五张数据表的迁移/模型骨架；
2. 实现一条“事件 → outbox → 发送卡片”的串联链路；
3. 实现“卡片回调 → 去重 → 授权 → 业务操作 → 结果回执”的串联链路；
4. 加入重试、死信、解绑和过期授权处理；
5. 再评估是否需要卡片更新、更多按钮或第二个渠道。

## 八、演进约束

本草案的主干是“一个硬件用户 → 企业微信应用单聊 → 一张可操作模板卡片”。后续若扩展微信，必须作为独立渠道接入：重新验证其主动通知、用户身份绑定与可操作卡片能力，新增微信适配器和对应的授权/投递实现，不复用企业微信的凭证、回调格式或卡片协议。

当前仅在 `im_bindings.platform` 中保留 `wecom` 标识，为后续迁移和审计留出边界；不提前实现多渠道路由、微信代码、群聊或文本命令。任何这类变更均应新写一份架构变更说明，明确新增需求、模块边界变化、数据迁移和幂等/授权规则。
