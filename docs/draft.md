# 一期 IM 数据模型与接口

> 状态：设计草案。
>
> 一期使用企业微信智能机器人 WebSocket；保留通用 `ImPlatformAdapter`，后续可增加其他平台适配器。
>
> 本文只定义 IM、设备绑定和设备投递的数据模型与接口。日程、提醒和语音业务不属于 IM Gateway。

## 一、数据模型

### 1. 设备 `devices`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 内部设备 ID，主键 |
| `device_id` | string | ESP32 稳定设备标识，唯一 |
| `device_name` | string | 设备展示名称 |
| `status` | enum | `unbound`、`pairing`、`bound`、`retired` |
| `created_at` / `updated_at` | datetime | 审计时间 |

约束：一个非 `retired` 设备最多存在一个有效绑定。

### 2. 配对会话 `pairing_sessions`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 配对会话 ID，主键 |
| `device_id` | string | 关联设备 |
| `code_hash` | string | 一次性配对码哈希 |
| `status` | enum | `pending`、`awaiting_confirmation`、`confirmed`、`cancelled`、`expired`、`rejected` |
| `external_user_id` | string? | 输入配对码的平台用户 ID |
| `confirmation_task_id` | string? | 确认卡片的平台任务 ID |
| `attempt_count` | integer | 校验失败次数 |
| `expires_at` | datetime | 过期时间 |
| `confirmed_at` | datetime? | 确认时间 |
| `created_at` / `updated_at` | datetime | 审计时间 |

约束：

- 同一设备最多一个未过期的 `pending` 或 `awaiting_confirmation` 会话；
- 只保存配对码哈希，不保存明文；
- 确认者必须等于 `external_user_id`；
- 成功、取消、过期或超过尝试上限后不可复用。

### 3. IM 绑定 `im_bindings`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 绑定 ID，主键 |
| `user_id` | string | VoiceLife 内部用户 ID |
| `device_id` | string | 关联设备 |
| `platform` | string | 一期为 `wecom` |
| `account_id` | string | Gateway 中的平台账号配置 ID |
| `external_user_id` | string | 平台用户 ID；企微为 `userid` |
| `external_conversation_id` | string? | 平台会话 ID |
| `status` | enum | `active`、`unbound`、`revoked` |
| `bound_at` | datetime | 绑定时间 |
| `unbound_at` | datetime? | 解绑时间 |
| `created_at` / `updated_at` | datetime | 审计时间 |

约束：

- `(platform, account_id, external_user_id)` 最多一个 `active` 绑定；
- `device_id` 最多一个 `active` 绑定；
- 平台身份使用 `platform + account_id + external_user_id`，不能使用昵称；
- 解绑保留历史记录。

### 4. IM 卡片 `im_cards`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 卡片 ID，主键 |
| `business_event_id` | string | 业务事件 ID，逻辑引用 |
| `binding_id` | string | 关联 IM 绑定 |
| `kind` | enum | `reminder_due`、`action_result`、`daily_schedule`、`binding_confirmation` |
| `platform_task_id` | string | 平台任务 ID；企微为 `task_id` |
| `content` | json | 平台无关卡片快照 |
| `status` | enum | `pending`、`sending`、`accepted`、`failed`、`dead_letter` |
| `external_message_id` | string? | 平台消息 ID |
| `attempt_count` | integer | 投递次数 |
| `next_attempt_at` | datetime? | 下次重试时间 |
| `expires_at` | datetime? | 失效时间 |
| `created_at` / `updated_at` | datetime | 审计时间 |

约束：

- `(business_event_id, binding_id, kind)` 唯一；
- `platform_task_id` 在同一平台账号内唯一；
- 投递重试必须复用同一个 `platform_task_id`；
- `accepted` 只表示平台接受消息，不表示用户已读。

### 5. 卡片操作 `im_card_actions`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 操作 ID，主键 |
| `card_id` | string | 关联卡片 |
| `action_type` | enum | `acknowledge`、`snooze_10_minutes`、`bind_confirm`、`bind_cancel` |
| `action_key_hash` | string | 按钮 key 哈希 |
| `actor_user_id` | string? | 允许操作的内部用户 ID |
| `operation_id` | string | 调用业务服务的稳定幂等键 |
| `callback_message_id` | string? | 平台事件 ID；企微为 `msgid` |
| `user_id` | string? | 实际点击者的平台用户 ID |
| `status` | enum | `pending`、`received`、`processing`、`executed`、`rejected`、`retryable_failed`、`expired` |
| `received_at` / `processed_at` | datetime? | 处理时间 |
| `expires_at` | datetime | 授权失效时间 |
| `created_at` | datetime | 创建时间 |

约束：

- `action_key_hash` 唯一；
- `(platform, account_id, callback_message_id)` 在平台账号范围内唯一；
- 处理和恢复始终复用 `operation_id`；
- 点击者必须与绑定用户一致；
- 重复事件返回既有结果，不重复执行业务操作。

### 6. 设备连接 `device_connections`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 连接记录 ID，主键 |
| `device_id` | string | 关联设备 |
| `connection_id` | string | Gateway 连接 ID |
| `status` | enum | `connecting`、`online`、`offline`、`closed` |
| `connected_at` | datetime | 建连时间 |
| `last_seen_at` | datetime | 最近心跳时间 |
| `closed_at` | datetime? | 关闭时间 |
| `close_reason` | string? | 关闭原因 |

约束：同一设备最多一个 `online` 连接；新连接认证成功后旧连接失效。

### 7. 设备消息 `device_messages`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 内部记录 ID，主键 |
| `message_id` | string | 设备协议幂等 ID，唯一 |
| `device_id` | string | 关联设备 |
| `payload` | json | 消息体 |
| `status` | enum | `pending`、`sent`、`acked`、`failed`、`expired` |
| `attempt_count` | integer | 投递次数 |
| `next_attempt_at` | datetime? | 下次重试时间 |
| `expires_at` | datetime? | 过期时间 |
| `created_at` / `acked_at` | datetime? | 审计时间 |

约束：设备按 `message_id` 去重；重复消息不重复执行但仍返回 ACK；`acked` 只表示设备收到消息。

---

## 二、内部模块接口

> 以下是进程内 Port，不是 HTTP API 或设备 WebSocket 协议。

### 1. 平台适配器

```ts
type ImPlatform = "wecom" | "feishu" | "dingtalk" | "wechat";

interface ImPlatformAdapter {
  readonly platform: ImPlatform;
  readonly accountId: string;

  start(): Promise<void>;
  stop(): Promise<void>;
  sendText(input: SendTextInput): Promise<SendMessageResult>;
  sendCard(input: SendCardInput): Promise<SendMessageResult>;
  updateCard(input: UpdateCardInput): Promise<void>;
  onMessage(handler: (message: NormalizedImMessage) => Promise<void>): void;
  onCardEvent(handler: (event: NormalizedCardEvent) => Promise<void>): void;
  getCapabilities(): ImCapabilities;
}

// 不同平台适配能力不同，根据能力进行降级
interface ImCapabilities {
  receiveText: boolean;
  sendText: boolean;
  sendCard: boolean;
  updateCard: boolean;
  cardActions: boolean;
  proactiveMessage: boolean;
  requiresPriorConversation: boolean;
  requiresPublicCallbackUrl: boolean;
}
```

一期实现：

```ts
class WeComAdapter implements ImPlatformAdapter {
  readonly platform = "wecom" as const;
  // 使用企业微信智能机器人 WebSocket SDK 实现。
}
```

### 2. 业务事件映射 IM

```ts
interface ImAssistService {
  projectBusinessEvent(input: {
    businessEventId: string;
  }): Promise<{ cardId?: string; duplicate: boolean }>;

  recordCardEvent(input: {
    event: NormalizedCardEvent;
  }): Promise<{ actionId: string; duplicate: boolean }>;

  processCardAction(input: {
    actionId: string;
  }): Promise<{
    outcome: "executed" | "duplicate" | "rejected" | "retryable_failed";
    resultBusinessEventId?: string;
  }>;
}
```

### 3. 绑定服务

```ts
interface BindingService {
  startPairing(input: {
    deviceId: string;
    ttlSeconds?: number;
  }): Promise<{
    pairingId: string;
    displayCode: string;
    expiresAt: number;
  }>;

  requestBinding(input: {
    platform: ImPlatform;
    accountId: string;
    pairingCode: string;
    externalUserId: string;
    externalConversationId?: string;
    callbackMessageId: string;
  }): Promise<{
    status: "confirmation_required" | "invalid" | "expired" | "already_bound";
    confirmationCardId?: string;
  }>;

  confirmBinding(input: {
    cardEvent: NormalizedCardEvent;
  }): Promise<{
    status: "bound" | "cancelled" | "duplicate" | "rejected" | "expired";
    bindingId?: string;
    deviceId?: string;
  }>;

  unbind(input: {
    bindingId: string;
    operationId: string;
  }): Promise<{ status: "unbound" | "duplicate" | "rejected" }>;
}
```

### 4. 业务操作接口

```ts
interface BusinessActionService {
  execute(input: {
    operationId: string;
    userId: string;
    actionType: "acknowledge" | "snooze_10_minutes";
    targetType: string;
    targetId: string;
  }): Promise<{
    resultBusinessEventId: string;
    duplicate: boolean;
  }>;
}
```

重复调用必须复用 `operationId`，不得重复关闭或推迟提醒。

---
