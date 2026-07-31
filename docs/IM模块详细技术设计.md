# IM 模块 - 详细技术设计 V1

本文件基于 `IM模块设计文档.md` 中的产品逻辑，提供 IM 模块的 API 接口设计与数据库表结构设计，作为技术研发的直接参考。

技术实现采用 Koishi-Centric 架构：

- 所有平台 IM 消息与平台事件通过 Koishi Adapter 接入；
- IM Use Case Layer（绑定、投递、动作、回执）通过 `ImChannelPort` 使用通道能力；
- VoiceLife Koishi Plugin 负责 `Koishi Session`、`NormalizedImEvent` 和发送意图之间的转换；
- VoiceLife Koishi Plugin 实现 `ImChannelPort`；该 Port 只是代码边界，不是独立网关服务；
- 模板、卡片、二维码和平台回执由 Capability Plugin 补充；
- H5/小程序是 Action UI，经 `@koishijs/plugin-server` 进入 VoiceLife Koishi Plugin 的 Action Route，不登记为平台 Adapter；
- VoiceLife 业务服务不直接依赖 Koishi `Session`、`Bot` 或平台原始报文；
- Satori Server 仅作为可选的外部 HTTP/WebSocket 协议出口。

---

# API 接口设计（V1 - Koishi-Centric）

## 1. 接口总览

### 1.1 ChannelAccount 管理

- `POST   /v1/im/channel-accounts` —— 创建通道账号
- `GET    /v1/im/channel-accounts` —— 查询通道账号列表
- `GET    /v1/im/channel-accounts/{accountId}` —— 获取通道账号详情
- `PUT    /v1/im/channel-accounts/{accountId}` —— 更新通道配置或启停状态
- `GET    /v1/im/channel-accounts/{accountId}/health` —— 查询 Koishi Bot 与 Adapter 健康状态

### 1.2 配对与绑定管理

- `POST   /v1/im/pairing-sessions` —— 创建一次性配对会话
- `GET    /v1/im/pairing-sessions/{pairingSessionId}` —— 查询配对进度
- `GET    /v1/im/bindings` —— 查询用户或设备的 IM 绑定
- `DELETE /v1/im/bindings/{bindingId}` —— 取消绑定

### 1.3 通知与投递管理

- `POST   /v1/im/notifications` —— 提交通知意图并异步创建一个或多个单通道投递
- `GET    /v1/im/deliveries` —— 查询投递列表
- `GET    /v1/im/deliveries/{deliveryId}` —— 查询单个投递及回执
- `POST   /v1/im/deliveries/{deliveryId}/retry` —— 人工重试失败或死信投递

### 1.4 Koishi 内部事件

- `POST   /internal/v1/im/events` —— 接收 VoiceLife Koishi Plugin 产生的 `NormalizedImEvent`

同进程部署时，VoiceLife Koishi Plugin 直接调用相同的应用服务接口，不经过 HTTP。

路由规则：

- 各平台 Adapter 产生的 `binding.requested` 统一进入 `Binding Handler`；
- 普通消息不进入 `Binding Handler`；
- 原生卡片按钮优先通过 Koishi `interaction/button` 进入 `ReminderActionHandler`；
- 仅在 Koishi Gateway 独立部署时，才把相同动作转换为 `action.triggered` 并提交到内部事件接口。

### 1.5 用户提醒动作（Action UI 入口）

- `GET    /voicelife/reminder-actions/{token}` —— 展示当前 H5 Action UI
- `POST   /voicelife/reminder-actions/{token}` —— H5/小程序提交统一提醒动作

该接口是 VoiceLife 产品接口，不属于通用 IM API，也不属于任何平台 Adapter。它物理运行在 Koishi Runtime 的 `plugin-server` 上，逻辑归属 VoiceLife Koishi Plugin。

```text
H5 / 小程序
  → @koishijs/plugin-server
  → VoiceLife Koishi Plugin
  → Action Route
  → ReminderActionHandler
  → IM Application.Action

原生卡片
  → Platform Adapter / Capability Plugin
  → interaction/button
  → VoiceLife Koishi Plugin
  → ReminderActionHandler
  → IM Application.Action
```

两条链路统一为 `{ token, action, params? }`，由同一个 `ReminderActionHandler` 完成验签、版本校验、幂等与业务转发，再调用 `IM Application.Action`。H5 可以替换为小程序，不新增第二个微信公众号 Adapter。各平台原始回调地址不作为业务 API 对外开放。

## 2. 通用接口约定

1. HTTP JSON 字段使用 camelCase，数据库字段使用 snake_case。
2. 时间统一使用 ISO 8601，数据库内部保存 UTC。
3. 创建配对、提交通知、人工重试和执行动作必须支持 `Idempotency-Key`。
4. `X-Correlation-ID` 贯穿业务事件、通知、投递、平台消息、回执和用户动作。
5. Secret、Token、私钥不得出现在 API Body 中，只允许传递 `credentialRef`。
6. 普通管理接口只返回脱敏后的外部用户标识。

统一成功响应：

```json
{
  "data": {}
}
```

统一错误响应：

```json
{
  "code": "NO_ACTIVE_BINDING",
  "message": "用户没有可用的 IM 绑定",
  "requestId": "request-01"
}
```

## 3. 详细接口参数

### 1）创建通道账号

- 方法与路径：`POST /v1/im/channel-accounts`
- 说明：创建一个由 Koishi Adapter 管理的平台账号。
- 入参（Body）：
  - `platform`（string，必填）：`wechat_official`、`wecom`、`wecom_aibot`、`feishu` 或 `dingtalk`。
  - `tenantExternalId`（string，必填）：AppID、CorpID、TenantKey 等非密钥标识。
  - `koishiBotId`（string，必填）：Koishi Runtime 内的 Bot 标识。
  - `credentialRef`（string，必填）：KMS 或密钥服务中的凭据引用。
  - `connectionMode`（string，必填）：`webhook`、`websocket` 或 `both`。
  - `capabilityConfig`（object，可选）：模板 ID、卡片模板等非敏感能力配置。
  - `enabled`（boolean，可选，默认 true）：是否启用。
- 约束：
  - V1 不单独设置展示名称，管理界面使用 `platform + tenantExternalId` 展示和识别账号。
  - Koishi Adapter 由服务端根据 `platform` 映射和装配，不允许 API 调用方传入任意 Plugin 名称。
- 出参：`data` 为创建成功的 `ChannelAccount`。

### 2）查询通道账号

- 方法与路径：
  - `GET /v1/im/channel-accounts`
  - `GET /v1/im/channel-accounts/{accountId}`
- 查询参数：
  - `platform`（string，可选）：按平台过滤。
  - `status`（string，可选）：按 `active`、`disabled`、`error` 过滤。
- 出参：
  - 列表接口的 `data.items` 为通道账号数组。
  - 详情接口的 `data` 为单个通道账号。
  - 不返回 Secret、Token 和私钥。

### 3）更新通道账号

- 方法与路径：`PUT /v1/im/channel-accounts/{accountId}`
- 入参（Path）：
  - `accountId`（string，必填）：通道账号 ID。
- 入参（Body，字段均可选）：
  - `credentialRef`（string）：新的凭据引用。
  - `capabilityConfig`（object）：新的能力配置。
  - `enabled`（boolean）：启用或停用通道。
- 出参：`data` 为更新后的通道账号。

### 4）查询通道健康状态

- 方法与路径：`GET /v1/im/channel-accounts/{accountId}/health`
- 出参（Body）：
  - `accountId`（string）：通道账号 ID。
  - `koishiBotId`（string）：对应的 Koishi Bot。
  - `status`（string）：`up` 或 `down`。
  - `lastConnectedAt`（string，可选）：最近连接成功时间。
  - `lastErrorCode`（string，可选）：最近一次错误码。

### 5）创建配对会话

- 方法与路径：`POST /v1/im/pairing-sessions`
- 说明：为指定用户或设备创建一次性绑定入口。
- 请求头：
  - `Idempotency-Key`（string，必填）。
- 入参（Body）：
  - `deviceId`（string，必填）：设备 ID。
  - `userId`（string，可选）：已知的 VoiceLife 用户 ID。
  - `ttlSeconds`（integer，可选，默认 600）：配对有效期。
  - `allowedPlatforms`（array<string>，可选）：允许绑定的平台。
- 出参（Body）：
  - `pairingSessionId`（string）：配对会话 ID。
  - `displayCode`（string）：供用户输入的一次性配对码。
  - `bindingUrl`（string，可选）：H5 或小程序绑定链接。
  - `expiresAt`（string）：过期时间。
  - `status`（string）：初始为 `pending`。

### 6）查询配对进度

- 方法与路径：`GET /v1/im/pairing-sessions/{pairingSessionId}`
- 出参（Body）：
  - `pairingSessionId`（string）：配对会话 ID。
  - `status`（string）：`pending`、`awaiting_confirmation`、`confirmed`、`expired`、`cancelled` 或 `rejected`。
  - `bindingId`（string，可选）：绑定成功后生成的 `ImBinding` ID。
  - `expiresAt`（string）：过期时间。

### 7）查询绑定

- 方法与路径：`GET /v1/im/bindings`
- 入参（Query）：
  - `userId`（string，可选）：按用户查询。
  - `deviceId`（string，可选）：按设备查询。
  - `platform`（string，可选）：按平台查询。
  - `status`（string，可选）：默认查询 `active`。
- 出参：`data.items` 为 `ImBinding` 数组，外部用户 ID 只返回脱敏值。

### 8）取消绑定

- 方法与路径：`DELETE /v1/im/bindings/{bindingId}`
- 说明：用户主动解绑和管理员取消绑定在 V1 使用同一个接口，取消后统一置为 `unbound`。
- 入参（Path）：
  - `bindingId`（string，必填）。
- 出参（Body）：
  - `bindingId`（string）。
  - `status`（string）：`unbound`。
  - `unboundAt`（string）。

### 9）提交通知意图

- 方法与路径：`POST /v1/im/notifications`
- 说明：提交平台无关的 `NotificationIntent`，系统异步创建一个或多个 `Delivery`。
- 请求头：
  - `Idempotency-Key`（string，必填）。
  - `X-Correlation-ID`（string，必填）。
- 入参（Body）：
  - `businessEventId`（string，必填）：业务事件 ID。
  - `kind`（string，必填）：`reminder_due`、`binding_confirmation` 或 `action_result`。
  - `recipient`（object，必填）：VoiceLife 内部的业务接收对象，不是平台 OpenID、UserID 或会话 ID。
    - `userId`（string，必填）：系统据此查询该用户的有效 `ImBinding`。
    - `deviceId`（string，可选）：需要限定设备时传入。
  - `targetBindingIds`（array<string>，可选）：显式指定要投递的绑定。未传时只选择一个默认有效绑定；传入多个绑定时分别创建多个 `Delivery`。
  - `content`（object，必填）：
    - `title`（string，必填）。
    - `body`（string，可选）。
    - `dueAt`（string，可选）。
    - `metadata`（object，可选）。
  - `actions`（array<object>，可选）：
    - `type`（string）：`acknowledge`、`snooze`、`open_url`、`bind_confirm` 或 `bind_cancel`。
    - `label`（string）。
    - `params`（object，可选）。
  - `expiresAt`（string，可选）。
- 出参（Body）：
  - `businessEventId`（string）。
  - `correlationId`（string）。
  - `status`（string）：`accepted`。
  - `deliveries`（array<object>）：本次创建的 `Delivery` 列表。

每个 `Delivery` 只对应一个 `bindingId` 和一个 IM 通道。一次 `NotificationIntent` 默认生成一个 `Delivery`；只有显式传入多个 `targetBindingIds` 时才生成多个投递。

### 10）查询投递列表

- 方法与路径：`GET /v1/im/deliveries`
- 入参（Query）：
  - `businessEventId`（string，可选）。
  - `userId`（string，可选）。
  - `bindingId`（string，可选）。
  - `platform`（string，可选）。
  - `status`（string，可选）。
  - `limit`（integer，可选，默认 20）。
  - `cursor`（string，可选）：游标分页。
- 出参：
  - `data.items` 为投递摘要数组。
  - `data.nextCursor` 为下一页游标。

### 11）查询投递详情

- 方法与路径：`GET /v1/im/deliveries/{deliveryId}`
- 出参（Body）：
  - `deliveryId`（string）。
  - `businessEventId`（string）。
  - `correlationId`（string）。
  - `bindingId`（string）。
  - `platform`（string）。
  - `status`（string）。
  - `externalMessageId`（string，可选）。
  - `attempts`（array<object>）：发送尝试列表。
  - `receipts`（array<object>）：`delivered`、`failed` 回执列表。
  - `lastError`（object，可选）。

### 12）人工重试投递

- 方法与路径：`POST /v1/im/deliveries/{deliveryId}/retry`
- 说明：仅用于 `retryable_failed`、`permanent_failed` 或 `dead_letter`。
- 请求头：
  - `Idempotency-Key`（string，必填）。
- 出参（Body）：
  - `deliveryId`（string）。
  - `status`（string）：重新进入 `pending`。
  - `nextAttemptNo`（integer）：下一次尝试序号。
- 约束：重试创建新的 `DeliveryAttempt`，但不创建新的 `Delivery`、`ActionIntent` 或 `operationId`。

### 13）提交规范化事件

- 方法与路径：`POST /internal/v1/im/events`
- 说明：Koishi Gateway 独立部署时，由 VoiceLife Koishi Plugin 提交 `NormalizedImEvent`。
- 鉴权：仅允许已认证的 Koishi Gateway 服务身份调用。
- 入参（Body）：
  - `schemaVersion`（integer，必填）：当前为 1。
  - `eventId`（string，必填）：VoiceLife 内部事件 ID。
  - `externalEventId`（string，可选）：平台事件 ID 或稳定事件指纹。
  - `platform`（string，必填）。
  - `channelAccountId`（string，必填）。
  - `occurredAt`（string，可选）：平台事件时间。
  - `receivedAt`（string，必填）：系统接收时间。
  - `actor`（object，可选）：`ExternalIdentity`。
  - `conversation`（object，可选）：`ConversationRef`。
  - `type`（string，必填）：规范化事件类型。
  - `correlationId`（string，可选）。
  - `payload`（object，必填）：与 `type` 对应的强类型载荷。
  - `rawEventRef`（string，可选）：原始事件的加密存储引用。
- 出参（Body）：
  - `eventId`（string）。
  - `status`（string）：`accepted` 或 `duplicate`。
- 幂等键：`channelAccountId + externalEventId`。
- 路由：
  - `binding.requested` → 共享 `Binding Handler`；
  - `action.triggered` → `ReminderActionHandler`，仅用于跨进程或无法直接产生 Koishi `interaction/button` 的兼容场景；
  - `message.received` → Message Handler，不得隐式解析成提醒动作。

### 14）展示 Action UI

- 方法与路径：`GET /voicelife/reminder-actions/{token}`
- 说明：当前返回 H5“知道了/推迟”页面；未来可由小程序替代展示层。
- 约束：
  - Token 必须签名并包含 `actionId`、`deliveryId`、绑定摘要、过期时间和版本。
  - URL 中不得出现裸 OpenID、UserID 或 reminderId。
- 出参：HTML 页面。

### 15）执行 Action UI 动作

- 方法与路径：`POST /voicelife/reminder-actions/{token}`
- 说明：接收 H5/小程序提交的提醒动作，并调用统一 `ReminderActionHandler`。原生卡片不调用此 HTTP 接口，而是通过 `interaction/button` 进入同一 Handler。
- 请求头：
  - `Idempotency-Key`（string，必填）。
- 入参（Body）：
  - `action`（string，必填）：`dismiss` 或 `snooze`。
  - `params`（object，可选）：例如 `{"minutes": 10}`。
- 出参（Body）：
  - `status`（string）：`executed` 或 `duplicate`。
  - `operationId`（string）：业务动作幂等 ID。
  - `result`（object）：Reminder Service 返回的业务结果。
- 错误：
  - `ACTION_EXPIRED`：动作已过期。
  - `ACTION_INVALID`：动作已失效或被新版本替代。
  - `IDENTITY_MISMATCH`：用户身份与目标绑定不一致。

边界约束：

1. Action Route 只负责解析 HTTP 和返回 H5/小程序结果。
2. `ReminderActionHandler` 负责验签、规范化、幂等和转发。
3. `IM Application.Action` 接收规范化动作，并通过业务端口下发 Dismiss/Snooze 命令。
4. 不允许 H5、小程序和卡片回调分别实现提醒状态变更。

Demo 以单进程方式组合这些模块，但仍保持同样的依赖边界：

```text
Action Route / interaction/button
  → ReminderActionHandler
  → IM Application.Action
  → Reminder Command Port
  → VoiceLifeService / TimingTask Demo Adapter
```

`Binding Handler` 同理只调用 `IM Application.Binding`，平台 Adapter 必须先把
OpenID、UserID 等字段转换成 `ExternalIdentity`。生产拆分部署时替换 Port
实现即可，不允许让 Handler 回退为直接调用产品 Service。

---

# 三、数据库表结构设计

数据库使用关系模型。主键默认使用 UUID，时间统一保存 UTC，敏感平台标识采用“密文存储 + 哈希查询”。

## 1. 通道账号表（`im_channel_accounts`）

存储由 Koishi Runtime 管理的平台 Bot 和 Adapter 配置。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | ChannelAccount ID |
| `platform` | varchar(32) | Not Null | IM 平台 |
| `tenant_external_id` | varchar(128) | Not Null | AppID、CorpID、TenantKey 等 |
| `koishi_bot_id` | varchar(128) | Not Null | Koishi Bot 标识 |
| `credential_ref` | varchar(256) | Not Null | 凭据系统引用 |
| `connection_mode` | varchar(16) | Not Null | `webhook`、`websocket`、`both` |
| `capability_config` | jsonb | Nullable | 非敏感平台能力配置 |
| `status` | varchar(16) | Not Null | `active`、`disabled`、`error` |
| `health_status` | varchar(16) | Not Null | `up`、`down` |
| `last_connected_at` | timestamptz | Nullable | 最近连接成功时间 |
| `last_error_code` | varchar(64) | Nullable | 最近错误码 |
| `created_at` | timestamptz | Not Null | 创建时间 |
| `updated_at` | timestamptz | Not Null | 更新时间 |

约束与索引：

- 唯一约束：`(platform, tenant_external_id)`。
- 唯一约束：`koishi_bot_id`。
- 索引：`(status, platform)`。

## 2. 配对会话表（`im_pairing_sessions`）

存储一次性设备与 IM 身份配对过程。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | PairingSession ID |
| `device_id` | varchar(128) | Not Null | Device Service 设备 ID |
| `user_id` | varchar(128) | Nullable | User Service 用户 ID |
| `code_hash` | varchar(128) | Not Null, Unique | 配对码哈希 |
| `allowed_platforms` | jsonb | Nullable | 允许的平台 |
| `requested_identity_id` | uuid | Nullable, FK | 发起绑定的平台身份 |
| `status` | varchar(32) | Not Null | 配对状态 |
| `attempt_count` | integer | Not Null | 尝试次数，默认 0 |
| `expires_at` | timestamptz | Not Null | 过期时间 |
| `confirmed_at` | timestamptz | Nullable | 确认时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |
| `updated_at` | timestamptz | Not Null | 更新时间 |

约束与索引：

- 同一设备最多存在一个有效的 `pending/awaiting_confirmation` 会话。
- `attempt_count >= 0`。
- 索引：`(device_id, status)`、`expires_at`。

## 3. 外部身份表（`im_external_identities`）

存储用户在某个 `ChannelAccount` 下的平台身份。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | ExternalIdentity ID |
| `channel_account_id` | uuid | Not Null, FK | 关联通道账号 |
| `tenant_external_id` | varchar(128) | Nullable | 平台 Tenant 标识 |
| `external_user_id_ciphertext` | text | Not Null | 加密后的平台用户 ID |
| `external_user_id_hash` | varchar(128) | Not Null | 用于唯一约束和查询 |
| `display_name` | varchar(256) | Nullable | 展示名称，不作为身份依据 |
| `profile` | jsonb | Nullable | 最小化保存的平台资料 |
| `status` | varchar(16) | Not Null | `active`、`unreachable` |
| `last_seen_at` | timestamptz | Nullable | 最近出现时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |
| `updated_at` | timestamptz | Not Null | 更新时间 |

唯一约束：`(channel_account_id, external_user_id_hash)`。

## 4. IM 绑定表（`im_bindings`）

存储 VoiceLife 用户、设备与平台身份之间的确认关系。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | ImBinding ID |
| `user_id` | varchar(128) | Not Null | VoiceLife 用户 ID |
| `device_id` | varchar(128) | Nullable | VoiceLife 设备 ID |
| `external_identity_id` | uuid | Not Null, FK | 外部身份 |
| `external_conversation_id_ciphertext` | text | Nullable | 加密后的平台会话 ID |
| `external_conversation_id_hash` | varchar(128) | Nullable | 会话查询哈希 |
| `priority` | integer | Not Null | 渠道优先级，默认 100 |
| `status` | varchar(16) | Not Null | `active`、`unbound` |
| `bound_at` | timestamptz | Not Null | 绑定时间 |
| `unbound_at` | timestamptz | Nullable | 解绑时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |
| `updated_at` | timestamptz | Not Null | 更新时间 |

索引：`(user_id, status, priority)`、`(device_id, status)`。

## 5. 入站事件表（`im_inbound_events`）

存储 VoiceLife Koishi Plugin 产生的 `NormalizedImEvent`，用于去重、异步处理和审计。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | 内部 eventId |
| `schema_version` | smallint | Not Null | 事件结构版本，默认 1 |
| `channel_account_id` | uuid | Not Null, FK | 通道账号 |
| `external_event_id` | varchar(256) | Not Null | 平台事件 ID 或稳定指纹 |
| `event_type` | varchar(64) | Not Null | 规范化事件类型 |
| `external_identity_id` | uuid | Nullable, FK | 事件参与者 |
| `correlation_id` | varchar(128) | Nullable | 链路关联 ID |
| `conversation` | jsonb | Nullable | 规范化会话引用 |
| `payload` | jsonb | Not Null | 强类型事件载荷 |
| `raw_payload_ref` | varchar(256) | Nullable | 加密原始报文引用 |
| `status` | varchar(16) | Not Null | `received`、`processing`、`processed`、`failed` |
| `occurred_at` | timestamptz | Nullable | 平台事件时间 |
| `received_at` | timestamptz | Not Null | 系统接收时间 |
| `processed_at` | timestamptz | Nullable | 处理完成时间 |
| `error_code` | varchar(64) | Nullable | 处理错误码 |
| `created_at` | timestamptz | Not Null | 创建时间 |

约束与索引：

- 唯一约束：`(channel_account_id, external_event_id)`。
- 索引：`(status, received_at)`、`correlation_id`。

## 6. 投递表（`im_deliveries`）

存储一个 `NotificationIntent` 经一个绑定发送到一个平台的记录。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | Delivery ID |
| `business_event_id` | varchar(128) | Not Null | 业务事件 ID |
| `correlation_id` | varchar(128) | Not Null | 链路关联 ID |
| `business_object_type` | varchar(64) | Nullable | 如 `reminder_occurrence` |
| `business_object_id` | varchar(128) | Nullable | 业务对象 ID |
| `binding_id` | uuid | Not Null, FK | 目标绑定 |
| `channel_account_id` | uuid | Not Null, FK | 发送通道快照 |
| `kind` | varchar(64) | Not Null | 通知类型 |
| `semantic_payload` | jsonb | Not Null | 平台无关通知快照 |
| `presentation_type` | varchar(32) | Not Null | 卡片、模板、Action UI 或文本 |
| `status` | varchar(32) | Not Null | 投递状态 |
| `external_message_id` | varchar(256) | Nullable | 平台消息 ID |
| `accepted_at` | timestamptz | Nullable | 平台接受时间 |
| `delivered_at` | timestamptz | Nullable | 明确送达时间 |
| `expires_at` | timestamptz | Nullable | 投递过期时间 |
| `last_error_code` | varchar(64) | Nullable | 最近错误码 |
| `last_error_message` | text | Nullable | 脱敏错误信息 |
| `created_at` | timestamptz | Not Null | 创建时间 |
| `updated_at` | timestamptz | Not Null | 更新时间 |

约束与索引：

- 唯一约束：`(business_event_id, binding_id, kind)`。
- 索引：`(status, created_at)`、`(binding_id, created_at)`、`external_message_id`、`correlation_id`。

## 7. 投递尝试表（`im_delivery_attempts`）

`im_deliveries` 表示“一条业务通知发往一个绑定”的逻辑投递，重试期间始终保留同一个 `delivery_id`；`im_delivery_attempts` 表示每一次真实的平台发送 API 调用。首次发送和每次重试都会新增一条 Attempt，例如一次投递重试两次，会有 1 条 Delivery 和 3 条 Attempt。

分表是为了保留每次调用的请求参数、平台错误和耗时，避免重试时覆盖历史记录，同时让业务侧只查询 Delivery 就能获得当前汇总状态。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | DeliveryAttempt ID |
| `delivery_id` | uuid | Not Null, FK | 所属投递 |
| `attempt_no` | integer | Not Null | 从 1 开始 |
| `request_id` | varchar(128) | Not Null | 稳定平台请求 ID |
| `rendered_payload` | jsonb | Not Null | 脱敏后的平台载荷 |
| `status` | varchar(24) | Not Null | `sending`、`accepted`、`retryable_failed`、`permanent_failed` |
| `platform_code` | varchar(64) | Nullable | 平台错误码 |
| `platform_message` | text | Nullable | 脱敏错误信息 |
| `started_at` | timestamptz | Not Null | 开始时间 |
| `finished_at` | timestamptz | Nullable | 结束时间 |
| `next_attempt_at` | timestamptz | Nullable | 下一次重试时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |

约束与索引：

- 唯一约束：`(delivery_id, attempt_no)`。
- 唯一约束：`request_id`。
- 索引：`(status, next_attempt_at)`。

## 8. 投递回执表（`im_delivery_receipts`）

存储平台返回的最终投递证据。V1 只保留 `delivered` 和 `failed` 两种回执：平台 API 是否受理记录在 `im_delivery_attempts.status`，用户是否完成操作记录在 `im_actions`，不再作为投递回执阶段。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | DeliveryReceipt ID |
| `delivery_id` | uuid | Not Null, FK | 所属投递 |
| `attempt_id` | uuid | Nullable, FK | 对应发送尝试 |
| `stage` | varchar(16) | Not Null | `delivered`、`failed` |
| `dedupe_key` | varchar(256) | Not Null | 回执幂等键 |
| `external_event_id` | varchar(256) | Nullable | 平台回执事件 ID |
| `raw_event_ref` | varchar(256) | Nullable | 原始回执引用 |
| `detail` | jsonb | Nullable | 脱敏状态信息 |
| `occurred_at` | timestamptz | Nullable | 平台状态时间 |
| `received_at` | timestamptz | Not Null | 系统接收时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |

约束与索引：

- 唯一约束：`dedupe_key`。
- 索引：`(delivery_id, occurred_at)`。

## 9. 用户动作表（`im_actions`）

存储用户可执行的业务动作及执行结果。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | Action ID |
| `delivery_id` | uuid | Not Null, FK | 所属投递 |
| `action_type` | varchar(32) | Not Null | `acknowledge`、`snooze` 等 |
| `action_params` | jsonb | Nullable | 如 `{"minutes": 10}` |
| `action_key_hash` | varchar(128) | Not Null | Action Token 或平台 action key 哈希 |
| `operation_id` | varchar(128) | Not Null | 业务动作幂等 ID |
| `expected_identity_id` | uuid | Not Null, FK | 允许执行的身份 |
| `actual_identity_id` | uuid | Nullable, FK | 实际执行身份 |
| `external_event_id` | varchar(256) | Nullable | 平台事件 ID |
| `status` | varchar(32) | Not Null | 动作状态 |
| `result` | jsonb | Nullable | 业务执行结果 |
| `expires_at` | timestamptz | Not Null | 过期时间 |
| `received_at` | timestamptz | Nullable | 接收时间 |
| `processed_at` | timestamptz | Nullable | 完成时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |

约束与索引：

- 唯一约束：`operation_id`、`action_key_hash`。
- 索引：`(delivery_id, status)`、`(expires_at, status)`。

## 10. Outbox 事件表（`im_outbox_events`）

存储待可靠发布或异步执行的内部事件。

| 字段名 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `id` | uuid | PK | Outbox Event ID |
| `aggregate_type` | varchar(64) | Not Null | 聚合类型 |
| `aggregate_id` | varchar(128) | Not Null | 聚合 ID |
| `event_type` | varchar(128) | Not Null | 事件类型 |
| `payload` | jsonb | Not Null | 事件载荷 |
| `status` | varchar(16) | Not Null | `pending`、`published`、`failed` |
| `attempt_count` | integer | Not Null | 尝试次数，默认 0 |
| `next_attempt_at` | timestamptz | Nullable | 下一次执行时间 |
| `published_at` | timestamptz | Nullable | 发布时间 |
| `created_at` | timestamptz | Not Null | 创建时间 |

索引：`(status, next_attempt_at)`。

## 11. 实体关系

```text
im_channel_accounts 1 ── N im_external_identities
im_channel_accounts 1 ── N im_inbound_events
im_external_identities 1 ── N im_bindings
im_pairing_sessions 1 ── 0..1 im_bindings
im_bindings 1 ── N im_deliveries
im_deliveries 1 ── N im_delivery_attempts
im_deliveries 1 ── N im_delivery_receipts
im_deliveries 1 ── N im_actions
```

## 12. 核心幂等约束

| 场景 | 幂等键 |
|---|---|
| 创建配对会话 | `Idempotency-Key` |
| 平台入站事件 | `channel_account_id + external_event_id` |
| 创建渠道投递 | `business_event_id + binding_id + kind` |
| 平台发送调用 | `delivery_id + attempt_no` |
| 平台投递回执 | `dedupe_key` |
| 用户业务动作 | `operation_id` |
