# 一期 IM 数据模型与接口

> 状态：设计草案。
>
> 一期使用企业微信智能机器人 WebSocket；保留通用 `ImPlatformAdapter`，后续可增加其他平台适配器。
>
> 本文只定义 IM、设备绑定和设备投递的数据模型与接口。日程、提醒和语音业务不属于 IM Gateway。

## 一、数据模型

### 1. 设备 `devices`

| 字段                        | 类型      | 说明                                     |
| --------------------------- | --------- | ---------------------------------------- |
| `id`                        | string    | 内部设备 ID，主键                        |
| `device_id`                 | string    | ESP32 稳定设备标识，唯一                 |
| `device_name`               | string    | 设备展示名称                             |
| `hardware_model`            | string    | 硬件型号                                 |
| `device_secret_hash`        | string    | 设备认证密钥哈希                         |
| `firmware_version`          | string    | 固件版本                                 |
| `status`                    | enum      | `unbound`、`pairing`、`bound`、`retired` |
| `last_seen_at`              | datetime? | 最近有效心跳时间                         |
| `created_at` / `updated_at` | datetime  | 审计时间                                 |

约束：一个非 `retired` 设备最多存在一个有效绑定；在线状态由 `device_connections` 表示。

### 2. 配对会话 `pairing_sessions`

| 字段                         | 类型      | 说明                                                         |
| ---------------------------- | --------- | ------------------------------------------------------------ |
| `id`                         | string    | 配对会话 ID，主键                                            |
| `device_id`                  | string    | 关联设备                                                     |
| `code_hash`                  | string    | 一次性配对码哈希                                             |
| `status`                     | enum      | `pending`、`awaiting_confirmation`、`confirmed`、`cancelled`、`expired`、`rejected` |
| `requested_external_user_id` | string?   | 输入配对码的平台用户 ID                                      |
| `confirmation_task_id`       | string?   | 确认卡片的平台任务 ID                                        |
| `attempt_count`              | integer   | 校验失败次数                                                 |
| `expires_at`                 | datetime  | 过期时间                                                     |
| `confirmed_at`               | datetime? | 确认时间                                                     |
| `created_at` / `updated_at`  | datetime  | 审计时间                                                     |

约束：

- 同一设备最多一个未过期的 `pending` 或 `awaiting_confirmation` 会话；
- 只保存配对码哈希，不保存明文；
- 确认者必须等于 `requested_external_user_id`；
- 成功、取消、过期或超过尝试上限后不可复用。

### 3. IM 绑定 `im_bindings`

| 字段                        | 类型      | 说明                           |
| --------------------------- | --------- | ------------------------------ |
| `id`                        | string    | 绑定 ID，主键                  |
| `user_id`                   | string    | VoiceLife 内部用户 ID          |
| `device_id`                 | string    | 关联设备                       |
| `platform`                  | string    | 一期为 `wecom`                 |
| `account_id`                | string    | Gateway 中的平台账号配置 ID    |
| `external_user_id`          | string    | 平台用户 ID；企微为 `userid`   |
| `external_conversation_id`  | string?   | 平台会话 ID                    |
| `status`                    | enum      | `active`、`unbound`、`revoked` |
| `bound_at`                  | datetime  | 绑定时间                       |
| `unbound_at`                | datetime? | 解绑时间                       |
| `created_at` / `updated_at` | datetime  | 审计时间                       |

约束：

- `(platform, account_id, external_user_id)` 最多一个 `active` 绑定；
- `device_id` 最多一个 `active` 绑定；
- 平台身份使用 `platform + account_id + external_user_id`，不能使用昵称；
- 解绑保留历史记录。

### 4. IM 卡片 `im_cards`

| 字段                        | 类型      | 说明                                                         |
| --------------------------- | --------- | ------------------------------------------------------------ |
| `id`                        | string    | 卡片 ID，主键                                                |
| `business_event_id`         | string    | 业务事件 ID，逻辑引用                                        |
| `binding_id`                | string    | 关联 IM 绑定                                                 |
| `kind`                      | enum      | `reminder_due`、`action_result`、`daily_schedule`、`binding_confirmation` |
| `platform_task_id`          | string    | 平台任务 ID；企微为 `task_id`                                |
| `content`                   | json      | 平台无关卡片快照                                             |
| `status`                    | enum      | `pending`、`sending`、`accepted`、`failed`、`dead_letter`    |
| `external_message_id`       | string?   | 平台消息 ID                                                  |
| `attempt_count`             | integer   | 投递次数                                                     |
| `next_attempt_at`           | datetime? | 下次重试时间                                                 |
| `expires_at`                | datetime? | 失效时间                                                     |
| `created_at` / `updated_at` | datetime  | 审计时间                                                     |

约束：

- `(business_event_id, binding_id, kind)` 唯一；
- `platform_task_id` 在同一平台账号内唯一；
- 投递重试必须复用同一个 `platform_task_id`；
- `accepted` 只表示平台接受消息，不表示用户已读。

### 5. 卡片操作 `im_card_actions`

| 字段                           | 类型      | 说明                                                         |
| ------------------------------ | --------- | ------------------------------------------------------------ |
| `id`                           | string    | 操作 ID，主键                                                |
| `card_id`                      | string    | 关联卡片                                                     |
| `action_type`                  | enum      | `acknowledge`、`snooze_10_minutes`、`bind_confirm`、`bind_cancel` |
| `action_key_hash`              | string    | 按钮 key 哈希                                                |
| `actor_user_id`                | string?   | 允许操作的内部用户 ID                                        |
| `operation_id`                 | string    | 调用业务服务的稳定幂等键                                     |
| `callback_message_id`          | string?   | 平台事件 ID；企微为 `msgid`                                  |
| `external_user_id`             | string?   | 实际点击者的平台用户 ID                                      |
| `status`                       | enum      | `pending`、`received`、`processing`、`executed`、`rejected`、`retryable_failed`、`expired` |
| `received_at` / `processed_at` | datetime? | 处理时间                                                     |
| `expires_at`                   | datetime  | 授权失效时间                                                 |
| `created_at`                   | datetime  | 创建时间                                                     |

约束：

- `action_key_hash` 唯一；
- `(platform, account_id, callback_message_id)` 在平台账号范围内唯一；
- 处理和恢复始终复用 `operation_id`；
- 点击者必须与绑定用户一致；
- 重复事件返回既有结果，不重复执行业务操作。

### 6. 设备连接 `device_connections`

| 字段               | 类型      | 说明                                        |
| ------------------ | --------- | ------------------------------------------- |
| `id`               | string    | 连接记录 ID，主键                           |
| `device_id`        | string    | 关联设备                                    |
| `connection_id`    | string    | Gateway 连接 ID                             |
| `protocol_version` | string    | 设备协议版本                                |
| `status`           | enum      | `connecting`、`online`、`offline`、`closed` |
| `connected_at`     | datetime  | 建连时间                                    |
| `last_seen_at`     | datetime  | 最近心跳时间                                |
| `closed_at`        | datetime? | 关闭时间                                    |
| `close_reason`     | string?   | 关闭原因                                    |

约束：同一设备最多一个 `online` 连接；新连接认证成功后旧连接失效。

### 7. 设备消息 `device_messages`

| 字段                      | 类型      | 说明                                            |
| ------------------------- | --------- | ----------------------------------------------- |
| `id`                      | string    | 内部记录 ID，主键                               |
| `message_id`              | string    | 设备协议幂等 ID，唯一                           |
| `device_id`               | string    | 关联设备                                        |
| `direction`               | enum      | `to_device`、`from_device`                      |
| `message_type`            | string    | 如 `binding.result`、`voice.play`、`state.sync` |
| `payload`                 | json      | 消息体                                          |
| `status`                  | enum      | `pending`、`sent`、`acked`、`failed`、`expired` |
| `attempt_count`           | integer   | 投递次数                                        |
| `next_attempt_at`         | datetime? | 下次重试时间                                    |
| `expires_at`              | datetime? | 过期时间                                        |
| `created_at` / `acked_at` | datetime? | 审计时间                                        |

约束：设备按 `message_id` 去重；重复消息不重复执行但仍返回 ACK；`acked` 只表示设备收到消息。

### 8. 关系

```text
devices 1 ── N pairing_sessions
devices 1 ── N device_connections
devices 1 ── N device_messages
devices 1 ── N im_bindings（最多一个 active）
im_bindings 1 ── N im_cards
im_cards 1 ── N im_card_actions
business_event 1 ── N im_cards（逻辑引用，由业务服务持有）
```

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

### 2. 规范化事件

```ts
interface NormalizedImMessage {
  platform: ImPlatform;
  accountId: string;
  callbackMessageId: string;
  requestId?: string;
  externalUserId: string;
  externalConversationId?: string;
  conversationType: "single" | "group";
  messageType: "text" | "image" | "voice" | "file";
  text?: string;
  receivedAt: number;
}

interface NormalizedCardEvent {
  platform: ImPlatform;
  accountId: string;
  callbackMessageId: string;
  requestId?: string;
  platformTaskId?: string;
  actionKey: string;
  externalUserId: string;
  externalConversationId?: string;
  receivedAt: number;
}
```

### 3. IM 投影服务

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

### 4. 绑定服务

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

### 5. 业务操作接口

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

## 三、企业微信 Adapter 映射

| 企业微信字段                | 平台无关字段             |
| --------------------------- | ------------------------ |
| `body.msgid`                | `callbackMessageId`      |
| `headers.req_id`            | `requestId`              |
| `body.from.userid`          | `externalUserId`         |
| `body.chatid` 或单聊 userid | `externalConversationId` |
| `body.chattype`             | `conversationType`       |
| `body.event.task_id`        | `platformTaskId`         |
| `body.event.event_key`      | `actionKey`              |

一期约束：

- 绑定只接受单聊文本 `绑定 <配对码>`；
- 提醒和日程业务不解析 IM 自由文本；
- 卡片事件必须先去重、校验用户并持久化；
- 更新卡片使用原事件 `req_id` 和相同 `task_id`；
- 模板卡片事件需在 5 秒内更新；
- 同一 BotID 同时只运行一个有效 WebSocket 连接。

---

## 四、Gateway 与 ESP32 WebSocket 接口

### 1. 连接与公共结构

```text
wss://<gateway-host>/v1/devices/connect
```

必须使用 TLS；建连后的第一条消息必须是 `device.authenticate`；未认证连接不能发送其他消息。

```ts
interface DeviceFrame<T = unknown> {
  version: "1.0";
  type: string;
  message_id: string;
  device_id: string;
  timestamp: number;
  payload: T;
}
```

`message_id` 是幂等键，重试时保持不变。

### 2. 设备认证

ESP32 → Gateway：

```json
{
  "version": "1.0",
  "type": "device.authenticate",
  "message_id": "auth-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216000,
  "payload": {
    "credential": "<device-credential>",
    "firmware_version": "0.3.0"
  }
}
```

Gateway → ESP32：

```json
{
  "version": "1.0",
  "type": "device.authenticate.result",
  "message_id": "auth-result-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216001,
  "payload": {
    "authenticated": true,
    "connection_id": "conn-001",
    "heartbeat_interval_seconds": 30
  }
}
```

### 3. 心跳

ESP32 → Gateway：

```json
{
  "version": "1.0",
  "type": "device.heartbeat",
  "message_id": "heartbeat-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216030,
  "payload": { "uptime_seconds": 3600 }
}
```

### 4. 开始配对

ESP32 → Gateway：

```json
{
  "version": "1.0",
  "type": "pairing.start",
  "message_id": "pair-start-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216040,
  "payload": { "ttl_seconds": 300 }
}
```

Gateway → ESP32：

```json
{
  "version": "1.0",
  "type": "pairing.started",
  "message_id": "pair-started-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216041,
  "payload": {
    "pairing_id": "pair-001",
    "display_code": "K7M2-9QPX",
    "expires_at": 1785216341
  }
}
```

### 5. 绑定结果

Gateway → ESP32：

```json
{
  "version": "1.0",
  "type": "binding.result",
  "message_id": "binding-result-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216100,
  "payload": {
    "status": "bound",
    "binding_id": "binding-001"
  }
}
```

### 6. 消息 ACK

接收方 → 发送方：

```json
{
  "version": "1.0",
  "type": "message.ack",
  "message_id": "ack-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216101,
  "payload": {
    "acked_message_id": "binding-result-001",
    "status": "received"
  }
}
```

规则：重复消息不重复执行，但再次返回 ACK；ACK 只表示消息收到；超过 `expires_at` 的消息不再投递。

### 7. 错误消息

```json
{
  "version": "1.0",
  "type": "error",
  "message_id": "error-001",
  "device_id": "xiaozhi-0001",
  "timestamp": 1785216102,
  "payload": {
    "request_message_id": "pair-start-001",
    "code": "PAIRING_ALREADY_ACTIVE",
    "retryable": false,
    "message": "设备已有有效配对会话"
  }
}
```

| 错误码                    | 可重试 | 说明               |
| ------------------------- | ------ | ------------------ |
| `AUTH_FAILED`             | 否     | 设备认证失败       |
| `UNSUPPORTED_VERSION`     | 否     | 协议版本不支持     |
| `INVALID_MESSAGE`         | 否     | 消息结构无效       |
| `PAIRING_ALREADY_ACTIVE`  | 否     | 已有有效配对会话   |
| `PAIRING_EXPIRED`         | 否     | 配对会话已过期     |
| `DEVICE_RETIRED`          | 否     | 设备已吊销         |
| `TEMPORARILY_UNAVAILABLE` | 是     | Gateway 暂时不可用 |
