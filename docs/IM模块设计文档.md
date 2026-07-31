# IM模块需求与设计文档 

## 0. 比较对象

| 平台         | 微信公众号         | 企业微信机器人       | 飞书企业自建应用      | 钉钉应用机器人    |
| ---------- | ------------- | ------------- | ------------- | ---------- |
| 连接方式       | HTTPS Webhook | Websocket 长连接 | WebSocket 长连接 | Stream 长连接 |
| 是否需要公网地址回调 | 是             | 否             | 否             | 否          |
| 官方Node SDK | 接口            | 有             | 有             | 有          |
| 接收文本       | 是             | 是             | 是             | 是          |
| 接收语音       | 是，可获得媒体/识别结果  | 是，可获得转写       | 支持音频/媒体消息     | 支持媒体消息     |
| 原生卡片       | 模板消息不是通用互动卡片  | 支持模板卡片        | 支持消息卡片        | 支持互动卡片     |
| 原生按钮回调     | 否，使用 H5/小程序   | 是             | 是             | 是          |
| 原消息更新      | 不作为通用能力       | 支持更新模板卡片      | 支持卡片/消息更新     | 支持互动卡片更新   |
| 用户动作回执     | H5/小程序          | 卡片事件          | 卡片事件          | 卡片事件       |

方案比较

| 方案          | 优点              | 缺点                 | 当前判断       |
| ----------- | --------------- | ------------------ | ---------- |
| 全部原生适配器     | 能完整使用平台能力；链路可控  | 每个平台都要维护 SDK、鉴权与协议 | 可行，但实现较复杂  |
| 全部迁入 Koishi | 基础消息模型统一；插件生态丰富 | 平台能力可能缺失或走不同接入路径   | 采用，并增加能力适配 |

 

## 1. 核心目标

将 VoiceLife 的用户身份绑定、业务通知、用户动作和消息回执抽象为稳定的 IM 通道能力，使 MVP 可以只使用微信公众号，同时无需修改提醒业务即可扩展企业微信、飞书和钉钉。

架构约束：

1. 一个 IM 平台只保留一个 Koishi Adapter。
2. H5/小程序是同一渠道的 Action UI，不是第二个平台 Adapter。
3. 各平台 Adapter 将绑定相关输入规范化后统一交给 `Binding Handler`。
4. H5、小程序和原生卡片动作统一交给 `ReminderActionHandler`。
5. Action Route 和 `ReminderActionHandler` 物理运行在 Koishi Runtime，逻辑归属 VoiceLife Koishi Plugin，不属于通用 Koishi Gateway 能力。

 

## 2. 核心概念定义

定义系统核心实体，以更精确地匹配我们的业务模型：
1. 外部通信平台类型`ImPlatform`：同一供应商的不同接入产品应使用不同 platform
2. 通道账号 `ChannelAccount`：VoiceLife 中一套可独立鉴权和运行的平台应用配置。

   如：一个微信公众号 AppID、一个企业微信 BotID、 一个飞书 AppID + Tenant、一个钉钉 ClientID + Corp。

   通道账号保存凭据引用，不直接向业务层暴露 Secret。
3. 外部身份 `ExternalIdentity`：用户在某个通道账号中的平台身份：

```ts
interface ExternalIdentity {
     platform: ImPlatform;
     channelAccountId: string;
     tenantId?: string;
     externalUserId: string;
   }
```

   `externalUserId` 不能全局唯一。
4. IM 绑定 `ImBinding`： 内部用户、设备与外部身份之间的已确认关系。

```ts
interface ImBinding {
     id: string;
     userId: string;
     deviceId?: string;
     channelAccountId: string;
     externalUserId: string;
     externalConversationId?: string; // 对于微信公众号不需要，但是对于其他有群聊的需要
     status: "active" | "unbound";
   }
```
5. 会话 `ConversationRef`：平台内发送消息的目标：

```ts
   interface ConversationRef {
     type: "direct" | "group";
     externalConversationId?: string;
     externalUserId?: string;
   }
```
6. 业务通知意图 `NotificationIntent`：业务服务向 IM 模块提交的语义化通知，而不是平台 JSON。

```ts
interface NotificationIntent {
  businessEventId: string;
  kind: "reminder_due" | "binding_confirmation" | "action_result";
  recipient: {
    userId: string;
    deviceId?: string;
    preferredBindingId?: string;
  };
  content: {
    title: string;
    body?: string;
    dueAt?: string;
    metadata?: Record<string, unknown>;
  };
  actions?: ActionIntent[];
  expiresAt?: string;
  idempotencyKey: string;
}
```
7. 动作意图 `ActionIntent`：平台按钮或 Action UI（H5/小程序）可执行的业务语义：

```ts
type ActionIntent =
  | {
      kind: "command";
      type:
        | "acknowledge"
        | "snooze"
        | "bind_confirm"
        | "bind_cancel";
      label: string;
      params?: Record<string, unknown>;
    }
  | {
      kind: "link";
      type: "open_url";
      label: string;
      url: string;
    };
```

   平台映射：

| 语义          | 微信公众号   | 企业微信 | 飞书   | 钉钉   |
| ----------- | ------- | ---- | ---- | ---- |
| acknowledge | H5“知道了” | 卡片按钮 | 卡片按钮 | 卡片按钮 |
| snooze      | H5“推迟”  | 卡片按钮 | 卡片按钮 | 卡片按钮 |
| open_url    | 模板 URL  | 卡片链接 | 卡片链接 | 卡片链接 |
8. 投递 `Delivery`: 一次业务通知经一个绑定发送到一个平台的记录。
9. 回执 `DeliveryReceipt`

   统一回执阶段：

```ts
type ReceiptStage =
  | "delivered"
  | "acted"
  | "failed";
```

语义：
- `delivered`：平台明确表示已投递；
- `acted`：用户完成“知道了/推迟”等明确动作；
- `failed`：平台拒绝或最终失败。
10. 入站事件 `NormalizedImEvent`：平台事件进入业务系统时的统一格式，例如公众号绑定消息、扫码事件或飞书卡片点击。

```ts
interface NormalizedImEvent {
  eventId: string;
  platform: ImPlatform;
  channelAccountId: string;
  occurredAt: string;
  actor?: ExternalIdentity;
  conversation?: ConversationRef;
  type:
    | "message.received"
    | "action.triggered"
    | "delivery.updated"
    | "binding.requested";
  payload: Record<string, unknown>;
  rawEventRef?: string;
}
```
11. 通道能力 `ChannelCapabilities`

```ts
interface ChannelCapabilities {
  inboundTransport: "webhook" | "websocket" | "both";
  directMessage: boolean;
  groupMessage: boolean;
  proactiveMessage: boolean;
  nativeCard: boolean;
  nativeAction: boolean;
  messageUpdate: boolean;
  deliveryReceipt: boolean;
  readReceipt: boolean;
  supplementalActionUi: Array<"h5" | "mini_program">;
}
```

Adapter 声明平台原生能力，VoiceLife Koishi Plugin 注册 Action UI 能力，两者合并为通道能力。业务层禁止通过 `platform === "feishu"` 等条件分支判断。

## 3. 核心业务流程

系统的运作主要分为四大流程：

### 流程一：IM 身份绑定生命周期

此流程用于建立 VoiceLife 用户、设备与平台用户身份之间的可信关系。
1. 发起绑定：设备或业务服务生成一次性配对码或签名绑定链接，并关联 `userId` 和可选 `deviceId`。
2. 识别平台身份：
   - 用户在 `ChannelAccount` 中发送配对码，或打开 H5/小程序绑定链接。
   - 各平台 Adapter 将绑定码、扫码或平台绑定事件统一转换为 `ExternalIdentity`、`ConversationRef` 和 `binding.requested` 类型的 `NormalizedImEvent`。
   - `Binding Handler` 消费规范化事件，不包含微信、飞书等平台判断。
3. 确认绑定：
   - 系统校验配对码、外部身份和有效期。
   - 通过包含 `bind_confirm`、`bind_cancel` 的 `NotificationIntent` 和 `ActionIntent` 展示确认入口。
4. 保存绑定：用户确认后创建 `ImBinding` 并设为 `active`；解绑设为 `unbound`。

### 流程二：业务提醒投递

此流程用于把 Reminder Service 产生的业务提醒转换为不同平台可以发送的消息。
1. 生成通知：IM 模块消费 `reminder.due`，创建平台无关的 `NotificationIntent`。
2. 选择通道：
   - 根据接收人查找有效的 `ImBinding`、`ChannelAccount` 和 `ConversationRef`。
   - 根据 `ChannelCapabilities` 选择原生卡片，或模板/文本加配置的 H5/小程序 Action UI。
3. 创建并发送：
   - 每个目标绑定生成一个 `Delivery`。
   - Renderer 将 `NotificationIntent` 和 `ActionIntent` 转换为平台内容，Adapter 执行发送并记录 `DeliveryAttempt`。
4. 处理结果：平台接受或拒绝后生成 `DeliveryReceipt`；临时失败进行重试，永久失败进入死信。

### 流程三：平台消息与提醒动作分流

平台消息与提醒动作共享业务语义，但入口边界不同。

1. 平台消息：
   - Adapter 验签、解密并接收 Webhook、WebSocket 或 Stream；
   - 绑定相关输入转换为 `binding.requested`，统一进入 `Binding Handler`；
   - 微信公众号文字只用于绑定，不解析“知道了/推迟”等提醒动作。
2. H5/小程序动作：
   - Action UI 经 `@koishijs/plugin-server` 向 VoiceLife Koishi Plugin 的 Action Route 提交 `{ token, action }`；
   - Action Route 不经过平台 Adapter，也不构造 Koishi Session。
3. 原生卡片动作：
   - Adapter 或 Capability Plugin 产生标准 `interaction/button`；
   - 事件转换为相同的 `{ token, action }`。
4. 统一执行：
   - `ReminderActionHandler` 验证令牌、动作和提醒版本；
   - 调用 `IM Application.Action`，再通过业务端口下发“知道了/推迟”命令；
   - 卡片更新原消息，H5/小程序展示结果，并记录 `acted` 回执。

### 流程四：平台回执与投递状态更新

此流程用于把不同平台的发送状态统一更新到系统投递记录。
1. 接收回执：Adapter 将平台发送结果转换为 `delivery.updated` 类型的 `NormalizedImEvent`。
2. 关联投递：通过平台消息 ID 或 `correlationId` 找到对应的 `Delivery` 和 `DeliveryAttempt`。
3. 更新状态：
   - 将平台结果映射为 `accepted`、`delivered`、`read`、`acted` 或 `failed` 的 `DeliveryReceipt`。
   - Receipt Service 幂等更新 `Delivery`，且不允许乱序回执造成状态倒退。

---

## 4. 模块架构

```text
微信公众号 IM ──→ WeChat Adapter ───────────────┐
企业微信 ───────→ WeCom Adapter ────────────────┤
飞书 ─────────→ Lark Adapter ──────────────────┼─→ Binding Handler
钉钉 ─────────→ DingTalk Adapter ───────────────┘      ↓
                                              Binding Service

Action UI（H5/小程序）
  → plugin-server
  → VoiceLife Koishi Plugin / Action Route ─────┐
                                                ├─→ ReminderActionHandler
未来原生卡片 → Adapter → interaction/button ────┘          ↓
                                                   IM Application.Action
                                                            ↓
                                                Reminder Command Port
```

图中的 `Binding Handler` 是共享应用入口，不是平台 Adapter。未来平台 Adapter 只有在收到绑定相关消息或事件时才指向它；普通聊天消息仍进入 Message Handler，卡片动作进入 `ReminderActionHandler`。

当前 Demo 采用单进程组合部署，但代码依赖方向与上图一致：

```text
Adapter / Action Route
  → Handler
  → IM Application
  → Binding Service Port / Reminder Command Port
  → Demo 业务实现
```

单进程不等于直接调用：Handler 不得依赖 `VoiceLifeService`。生产拆分进程时，
只替换 Port 的 IPC/RPC 实现，不修改 Adapter、Action Route 或 Handler。

### 4.1 组件职责

| 组件 | 职责 | 不负责 |
|---|---|---|
| Platform Adapter | 连接平台、验签解密、生成 Koishi Session/标准事件 | 绑定规则、提醒状态机 |
| Capability Plugin | 模板、二维码、原生卡片、平台回执等专属能力 | 通用业务动作 |
| Binding Handler | 接收规范化绑定事件、校验配对信息、调用 Binding Service | 平台报文解析、提醒动作 |
| `plugin-server` | 为 VoiceLife Plugin 承载 HTTP Route | 产品动作语义 |
| Action Route | 接收 H5/小程序提交的签名动作 | 修改提醒状态 |
| `interaction/button` Handler | 把原生按钮事件转换为统一动作输入 | 平台无关业务规则 |
| `ReminderActionHandler` | 验签、规范化、幂等并转发提醒动作 | H5 渲染、卡片渲染 |

### 4.2 Binding Handler 输入契约

所有平台使用同一绑定输入：

```ts
interface BindingRequest {
  eventId: string;
  channelAccountId: string;
  actor: ExternalIdentity;
  conversation?: ConversationRef;
  pairingCode?: string;
  scene?: string;
}
```

规则：

1. Adapter 负责从平台字段提取 `pairingCode` 或 `scene`。
2. Binding Handler 不读取微信 OpenID、飞书 OpenID 等平台专属字段，只读取 `ExternalIdentity`。
3. 重复的 `eventId` 必须幂等。
4. 没有绑定语义的普通消息不得进入 Binding Handler。

### 4.3 微信公众号双入口

```text
微信公众号渠道
├─ IM 入口
│  └─ WeChat Adapter → Binding Handler / 通知投递
└─ Action UI
   └─ H5（未来可换小程序）
      → plugin-server
      → VoiceLife Koishi Plugin / ReminderActionHandler
      → IM Application.Action
```

微信公众号只有一个 WeChat Adapter。H5/小程序只是同一渠道的交互能力补充；它绕过 WeChat Adapter，但不绕过 Koishi Runtime 中的 VoiceLife Koishi Plugin。替换 UI 不改变 Adapter 数量，也不复制提醒动作逻辑。

VoiceLife 产品路由为：

- `GET /voicelife/reminder-actions/{token}`
- `POST /voicelife/reminder-actions/{token}`

该路由不属于通用 `/v1/im/*` 接口。

### 4.4 未来平台接入规则

新增企业微信、飞书或钉钉时：

1. 安装或实现对应 Koishi Adapter。
2. 将绑定相关事件映射为 `binding.requested`，接入现有 Binding Handler。
3. 将原生按钮映射为 `interaction/button`，接入现有 `ReminderActionHandler`。
4. 只新增平台渲染与 Capability Plugin，不新增平台专属 Binding Service 或 Reminder Action Service。
