# 硬件语音助手的 IM 辅助通道架构草案

> 状态：一期架构草案。本文只描述“硬件语音主交互 + 企业微信单聊辅助”的主干，不以现有 live demo 或已有代码为依据。

## 一、需求与边界

产品运行在硬件上，用户主要通过语音完成交互。IM 只是语音播报的辅助通道：提供可回看、可点击的卡片，不是第二个聊天入口。

一期支持：

- 企业微信智能机器人 WebSocket；
- 一个硬件用户绑定一个企业微信成员，使用一对一会话；
- 业务事件主动发送模板卡片；
- 卡片按钮触发“知道了”“10 分钟后提醒”等受控操作；
- 用户查询今日日程时，语音播报摘要，同时发送日程卡片。

一期不支持群聊、文本指令、多 IM 平台路由、聊天记录同步、已读状态和附件。

## 二、核心决策与风险

| 决策 | 结论 |
| --- | --- |
| 主入口 | 硬件语音；IM 仅辅助。 |
| 一期平台 | 企业微信智能机器人 WebSocket，便于公司内部用户联调。 |
| 状态归属 | 业务服务是提醒、日程和操作结果的唯一事实来源。 |
| 双通道关系 | 同一业务事件并行驱动语音和 IM；任何一侧失败不阻塞另一侧或业务状态。 |
| 上行方式 | 业务操作只接受模板卡片按钮回调；设备绑定允许严格格式的“绑定 <配对码>”系统文本命令，不解析 IM 自由文本执行提醒或日程操作。 |
| 使用Gateway接企业微信 | 实现简单，便于后续支持多种 IM 平台 |

一期支持：

- 一台 ESP32-S3 设备绑定一个企业微信用户；
- 一个企业微信用户绑定一台主要设备；
- 企业微信智能机器人 WebSocket 长连接；
- 文本消息接收；
- 模板卡片回复；
- 模板卡片按钮点击事件；
- 卡片更新；
- LED 显示配对码、绑定结果和连接状态；
- 设备主动连接 Gateway；
- 设备断线重连；
- 绑定、解绑和重新绑定；
- 卡片事件幂等处理。

一期不支持

- 扫码绑定；
- 群聊中绑定设备；
- 一个设备同时绑定多个用户；
- 一个机器人多实例双活连接；
- 让 ESP32 直接实现完整企业微信协议；
- 通过企业微信自由文本直接执行高风险业务操作；
- 用户仅凭设备编号远程抢占设备；
- 未经确认自动替换已有绑定；
- 把企业微信智能机器人 WebSocket 和企业微信自建应用 HTTP 回调混用。

## 三、模块架构

业务服务在修改业务状态时，可靠地记录一条业务事件。提交后，内部任务将同一事件分别投影到硬件语音通道和 IM 辅助模块。两个通道独立投递、重试和记录失败，任何一侧失败不阻塞另一侧或回滚业务状态；这个内部事件分发任务是业务服务的实现细节，不单独展开为架构模块。

```text
┌──────────────────────────────┐
│        企业微信客户端         │
│  用户消息 / 绑定卡片 / 按钮    │
└──────────────┬───────────────┘
               │
               ▼
┌──────────────────────────────────────┐
│ 企业微信智能机器人                    │
│ BotID + 长连接专用 Secret             │
└──────────────┬───────────────────────┘
               │ 单一 WSS 长连接
               ▼
┌──────────────────────────────────────┐
│ IM Gateway                           │
│                                      │
│  WeCom Adapter                       │
│  ├─ WebSocket 认证、心跳、重连        │
│  ├─ 文本消息接收                      │
│  ├─ 模板卡片发送                      │
│  ├─ 卡片事件接收与更新                │
│  └─ 企业微信消息去重                  │
│                                      │
│  Binding Service                      │
│  ├─ 配对码生成与校验                  │
│  ├─ 绑定确认                          │
│  ├─ 解绑与换绑                        │
│  └─ 用户 / 设备关系维护               │
│                                      │
│  Device Gateway                       │
│  ├─ 设备认证                          │
│  ├─ 设备连接管理                      │
│  ├─ 绑定结果通知                      │
│  ├─ 设备消息投递                      │
│  └─ 离线队列与重连补偿                │
│                                      │
│  Persistence                          │
│  ├─ devices                           │
│  ├─ pairing_sessions                   │
│  ├─ im_bindings                        │
│  ├─ device_connections                 │
│  ├─ device_messages                    │
│  └─ binding_actions                    │
└──────────────┬───────────────────────┘
               │ 设备主动建立 TLS 连接
               ▼
┌──────────────────────────────────────┐
│ ESP32-S3 小智机器人                  │
│                                      │
│  Device Client                        │
│  ├─ 设备认证                          │
│  ├─ 心跳                              │
│  ├─ 重连                              │
│  ├─ 绑定模式                          │
│  └─ 消息收发                          │
│                                      │
│  UI / Voice                           │
│  ├─ LED 显示配对码                    │
│  ├─ LED 显示绑定结果                  │
│  ├─ 按键                              │
│  ├─ ASR / TTS                         │
│  └─ 本地设备控制                      │
└──────────────────────────────────────┘
```

当前企业微信 Demo 使用 Node.js SDK：

```text
@wecom/aibot-node-sdk
```

该 SDK 运行在 Node.js 环境，而ESP32-S3 固件通常使用 C/C++ 或 ESP-IDF，不能直接运行 Node.js 依赖，出于开发难度考虑，建议增加 Gateway 。

## 四、平台无关的数据模型

以下是一期必须落地的逻辑模型。它们可以实现为四张表，也可以在同一数据库中按服务边界拆分；本文固定字段语义与约束，不限制具体 ORM 或存储引擎。

### 1. 设备主表 `devices`

| 字段 | 说明 |
| --- | --- |
| `id` | 内部设备 ID |
| `device_id` | ESP32 的稳定设备标识，唯一 |
| `device_name` | 展示名，例如“小智-1234” |
| `hardware_model` | 例如 `esp32-s3-xiaozhi` |
| `status` | `unregistered`、`online_unbound`、`pairing`、`online_bound`、`offline_bound`、`retired` |
| `device_secret_hash` | 设备认证 Secret 的哈希 |
| `firmware_version` | 固件版本 |
| `last_seen_at` | 最近心跳时间 |
| `created_at` | 创建时间 |
| `updated_at` | 更新时间 |

约束：

- `device_id` 唯一；
- `device_secret_hash` 不进入普通业务日志；
- `retired` 设备不能重新使用原设备身份。

### 2. 一次性配对会话 `pairing_sessions`

| 字段 | 说明 |
| --- | --- |
| `id` | 配对会话 ID |
| `device_id` | 目标设备 |
| `code_hash` | 配对码哈希 |
| `status` | `pending`、`confirmed`、`expired`、`cancelled`、`rejected` |
| `requested_external_user_id` | 发起绑定消息的企业微信 userid |
| `confirmation_task_id` | 确认卡片的 task_id |
| `attempt_count` | 校验失败次数 |
| `expires_at` | 过期时间 |
| `confirmed_at` | 确认时间 |
| `created_at` | 创建时间 |
| `updated_at` | 更新时间 |

约束：

- 同一设备最多一个未过期 `pending` 会话；
- `code_hash` 不保存明文配对码；
- `confirmation_task_id` 唯一；
- 过期、取消、确认后不能再次使用；
- 绑定确认必须由发起配对消息的同一 `userid` 完成。


### 3. 用户绑定 `im_bindings`

| 字段 | 说明 |
| --- | --- |
| `id` | 内部绑定 ID |
| `user_id` | VoiceLife 内部用户 ID |
| `device_id` | ESP32 设备 ID |
| `platform` | 一期固定为 `wecom` |
| `external_user_id` | 企业微信 userid |
| `external_conversation_id` | 企业微信单聊会话标识 |
| `status` | `active`、`unbound`、`revoked` |
| `bound_at` | 绑定时间 |
| `unbound_at` | 解绑时间 |
| `created_at` | 创建时间 |
| `updated_at` | 最后更新时间 |

约束：

- `(platform, external_user_id)` 最多一个 active 绑定；
- `device_id` 最多一个 active 主绑定；
- 解绑采用状态变更，不物理删除历史记录；
- 用户身份使用 `external_user_id`，不使用昵称。

### 4. 设备连接状态与会话 `device_connections`

| 字段 | 说明 |
| --- | --- |
| `id` | 连接记录 ID |
| `device_id` | 设备 ID |
| `connection_id` | Gateway 生成的连接 ID |
| `status` | `connecting`、`online`、`offline`、`closed` |
| `client_version` | 设备协议版本 |
| `remote_address_hash` | 可选，脱敏后的网络审计信息 |
| `connected_at` | 建连时间 |
| `last_seen_at` | 最近心跳时间 |
| `closed_at` | 断开时间 |
| `close_reason` | 断开原因 |

同一设备只能有一个 active 设备连接。新连接建立时，旧连接应被关闭或标记为失效。

### 5. Gateway 与设备之间的可靠消息收发记录 `device_messages`

| 字段 | 说明 |
| --- | --- |
| `id` | 内部消息 ID |
| `device_id` | 目标设备 |
| `direction` | `to_device`、`from_device` |
| `message_id` | Gateway 生成的幂等消息 ID |
| `message_type` | `binding.result`、`im.event`、`business.event` 等 |
| `payload` | 规范化后的消息内容 |
| `status` | `pending`、`sent`、`acked`、`failed`、`expired` |
| `attempt_count` | 投递次数 |
| `next_attempt_at` | 下次重试时间 |
| `created_at` | 创建时间 |
| `acked_at` | 确认时间 |

约束：

- `message_id` 唯一；
- 设备重复收到同一个消息时不得重复执行；
- 设备 ACK 不等于业务操作成功；
- 业务执行结果仍由业务服务产生。

### 6.绑定卡片按钮事件的收件与幂等记录 `binding_actions`

| 字段 | 说明 |
| --- | --- |
| `id` | 内部操作 ID |
| `pairing_session_id` | 配对会话 |
| `task_id` | 企业微信模板卡片 task_id |
| `event_key` | `bind_confirm` 或 `bind_cancel` |
| `external_user_id` | 点击者 userid |
| `callback_msg_id` | 企业微信事件 msgid |
| `status` | `received`、`processing`、`executed`、`rejected`、`expired` |
| `operation_id` | 稳定业务幂等键 |
| `created_at` | 回调时间 |
| `processed_at` | 处理时间 |

唯一约束建议：

```text
(platform, callback_msg_id)
(task_id, event_key, external_user_id)
```

---

## 五、接口契约

企业微信智能机器人 WebSocket 适配器设计

企业微信智能机器人 Adapter 只处理企业微信协议，不处理日程和提醒业务。

```ts
interface WeComChannelAdapter {
  start(): Promise<void>;

  replyText(input: {
    requestId: string;
    content: string;
  }): Promise<void>;

  replyTemplateCard(input: {
    requestId: string;
    taskId: string;
    card: TemplateCard;
  }): Promise<void>;

  updateTemplateCard(input: {
    requestId: string;
    taskId: string;
    card: TemplateCard;
  }): Promise<void>;

  onTextMessage(
    handler: (message: NormalizedImMessage) => Promise<void>,
  ): void;

  onTemplateCardEvent(
    handler: (event: NormalizedCardEvent) => Promise<void>,
  ): void;
}
```

### 1. 规范化文本消息

```ts
interface NormalizedImMessage {
  platform: "wecom";
  callbackMessageId: string;
  requestId: string;
  externalUserId: string;
  externalConversationId?: string;
  conversationType: "single" | "group";
  text: string;
  receivedAt: number;
}
```

一期绑定只接受：

```text
conversationType === "single"
```

群聊中的绑定消息直接返回提示，不进入绑定流程。

### 2. 规范化卡片事件

```ts
interface NormalizedCardEvent {
  platform: "wecom";
  callbackMessageId: string;
  requestId: string;
  taskId: string;
  eventKey: string;
  externalUserId: string;
  receivedAt: number;
}
```

### 3. 企业微信时限

- 普通消息可使用 `replyTemplateCard` 返回卡片；
- 模板卡片点击事件必须在回调后 5 秒内调用 `updateTemplateCard`；
- `updateTemplateCard` 必须使用事件对应的 `requestId/req_id`；
- 更新卡片时 `task_id` 必须保持一致；
- 企业微信机器人同一时间只允许一个有效 WSS 连接；
- Gateway 需要维护心跳和自动重连。

收到回调后不能先等待 LLM 或业务服务完成再响应卡片事件。正确顺序是：

```text
接收事件
  ↓
校验 frame 字段、用户、卡片和状态
  ↓
可靠持久化
  ↓
5 秒内更新卡片或返回协议响应
  ↓
异步执行后续业务
```

## 六、主干流程

### 1. 绑定设备

1. 用户通过物理按键进入配对模式，向 Gateway 发送绑定请求。
2. Gateway 收到请求后，返回一次性配对码并使设备进入配对状态。
3. 用户使用企业微信向机器人单聊发送严格格式的 `绑定 <配对码>` 系统命令；该命令只用于配对码校验，不执行提醒或日程业务。
4. 配对码匹配成功后，发送绑定确认卡片“请确认将当前企业微信账号绑定到该设备”，提供“确认”“取消”按钮，等待用户确认。
5. 用户确认后校验 `task_id`、`event_key`、点击者 `userid`、配对会话有效期和设备状态；通过后建立绑定关系并可靠通知硬件。

### 2. 提醒到达

1. 业务服务完成提醒到期状态变更，并可靠记录“提醒到达”事件。
2. 事件分发任务分别创建硬件语音投影和 IM 投影；两条投影独立投递、重试和记录状态。
3. 硬件语音投影通过 Device Gateway 向绑定的 ESP32 投递播报消息；IM 投影创建提醒卡片和“知道了”“10 分钟后提醒”按钮，并通过 IM Gateway 异步投递到企业微信。
4. 任一通道投递失败只重试对应通道，不回滚提醒状态，也不阻塞另一通道。

### 3. 用户点击 IM 卡片

1. 企业微信通过 WebSocket 推送 `template_card_event`；适配器从 frame 中解析 `msgid`、`req_id`、`task_id`、`event_key` 和点击者 `userid`。
2. IM 模块去重并校验卡片、按钮、绑定用户和有效期后，持久化该操作。
3. 在事件回调后的 5 秒内，使用原始 `req_id` 和相同 `task_id` 更新卡片为“正在处理”或快速最终结果；业务操作不等待 LLM 或其他耗时任务。
4. 后台以该按钮固定的操作 ID 调用业务服务执行确认或推迟；重复点击只得到同一个业务结果。
5. 业务服务产生“操作完成”事件；IM 通道发送结果卡片，设备通道只接收状态同步，是否语音播报由事件来源和投递策略决定。

### 4. 用户查询今日日程

1. 用户通过硬件发起查询；业务服务按用户时区生成一次日程展示快照，并可靠记录查询结果事件。
2. 硬件语音投影通过 Device Gateway 向发起查询的设备播报摘要；IM 投影使用同一份快照向绑定的企业微信用户发送完整日程卡片。
3. 一期日程卡片只用于查看，不提供 IM 上行按钮。

### 5. 设备离线与恢复

1. Device Gateway 心跳超时后将设备标记为 `offline`，待发送设备消息进入有界队列。
2. IM 通道继续独立工作；业务事件不因设备离线而回滚。
3. 设备重新连接并认证后，Gateway 按消息 TTL 和业务有效性筛选待投递消息。
4. ESP32 以稳定 `message_id` 去重并返回设备 ACK；设备 ACK 只表示消息收到，不表示业务操作完成。

以下内容是实现要求，不是新增业务模块：

- 业务状态变更与业务事件必须一起成功或一起失败；常见实现是事务外盒（outbox）。
- 卡片事件在 5 秒响应窗口内必须完成必要的持久化和卡片更新；耗时业务放到异步任务。
- 平台响应、IM 事件持久化、业务操作完成和设备 ACK 是不同状态，不能统一称为 ACK。
- 每个按钮有一次性、短期有效的服务端标识；处理时必须原子认领，并始终复用同一个业务操作 ID。
- 业务事件、卡片和操作处理都必须支持重复投递；结果卡片只由“操作完成”业务事件生成一次。
- 企业微信 `task_id` 在重试和卡片更新时保持不变，避免回调无法关联历史卡片。

## 八、一期验收与演进

上线前以真实企业微信智能机器人长连接完成闭环验证：

1. 测试用户已先与智能机器人建立会话，能收到一对一模板卡片；
2. “知道了”“10 分钟后提醒”按钮能收到 `template_card_event`，并在 5 秒内更新卡片；
3. 重放同一事件、并发点击同一按钮、Gateway 在卡片更新后中断，均只产生一次业务变更；
4. 业务状态提交后、语音和 IM 分发前中断，重启后仍会补发两个通道；
5. 查询今日日程时，语音和 IM 使用同一份快照；
6. 设备离线期间，IM 投递和设备消息队列互不阻塞，设备恢复后只补发仍有效的消息。

后续如果接入微信，作为独立渠道新增适配器、身份绑定和卡片/回调协议；不复用企业微信的凭证、回调格式或 `task_id` 规则。多渠道路由、群聊和文本命令都必须在新的架构变更中单独评估。
