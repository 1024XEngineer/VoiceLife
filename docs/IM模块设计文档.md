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
| 用户动作回执     | H5/小程序/文字回复   | 卡片事件          | 卡片事件          | 卡片事件       |

方案比较

| 方案          | 优点              | 缺点                 | 当前判断       |
| ----------- | --------------- | ------------------ | ---------- |
| 全部原生适配器     | 能完整使用平台能力；链路可控  | 每个平台都要维护 SDK、鉴权与协议 | 可行，但实现较复杂  |
| 全部迁入 Koishi | 基础消息模型统一；插件生态丰富 | 平台能力可能缺失或走不同接入路径   | 采用，并增加能力适配 |

 

## 1. 核心目标

将 VoiceLife 的用户身份绑定、业务通知、用户动作和消息回执抽象为稳定的 IM 通道能力，使 MVP 可以只使用微信公众号，同时无需修改提醒业务即可扩展企业微信、飞书和钉钉。

 

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
7. 动作意图 `ActionIntent`：平台按钮或 H5 页面可执行的业务语义：

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
10. 入站事件 `NormalizedImEvent`：平台事件进入业务系统时的统一格式（微信公众号的按钮点击、飞书卡片点击等原始报文的转换）

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
}
```

能力必须由 Adapter 声明，禁止业务层通过 `platform === "feishu"` 等条件分支判断。

## 3. 核心业务流程

系统的运作主要分为四大流程：

### 流程一：IM 身份绑定生命周期

此流程用于建立 VoiceLife 用户、设备与平台用户身份之间的可信关系。
1. 发起绑定：设备或业务服务生成一次性配对码或签名绑定链接，并关联 `userId` 和可选 `deviceId`。
2. 识别平台身份：
   - 用户在 `ChannelAccount` 中发送配对码，或打开 H5/小程序绑定链接。
   - Adapter 将平台用户和会话转换为 `ExternalIdentity`、`ConversationRef` 和 `binding.requested` 类型的 `NormalizedImEvent`。
3. 确认绑定：
   - 系统校验配对码、外部身份和有效期。
   - 通过包含 `bind_confirm`、`bind_cancel` 的 `NotificationIntent` 和 `ActionIntent` 展示确认入口。
4. 保存绑定：用户确认后创建 `ImBinding` 并设为 `active`；解绑设为 `unbound`。

### 流程二：业务提醒投递

此流程用于把 Reminder Service 产生的业务提醒转换为不同平台可以发送的消息。
1. 生成通知：IM 模块消费 `reminder.due`，创建平台无关的 `NotificationIntent`。
2. 选择通道：
   - 根据接收人查找有效的 `ImBinding`、`ChannelAccount` 和 `ConversationRef`。
   - 根据 `ChannelCapabilities` 选择原生卡片、模板消息或文本加 H5。
3. 创建并发送：
   - 每个目标绑定生成一个 `Delivery`。
   - Renderer 将 `NotificationIntent` 和 `ActionIntent` 转换为平台内容，Adapter 执行发送并记录 `DeliveryAttempt`。
4. 处理结果：平台接受或拒绝后生成 `DeliveryReceipt`；临时失败进行重试，永久失败进入死信。

### 流程三：用户消息、按钮或 H5 动作处理

此流程用于统一处理平台普通消息、卡片按钮和 H5 操作。
1. 接收事件：Adapter 验签、解密并接收 Webhook、WebSocket、Stream 或 H5 请求。
2. 规范化事件：
   - 平台用户和会话转换为 `ExternalIdentity` 与 `ConversationRef`。
   - 平台报文转换为 `NormalizedImEvent`，并使用 `externalEventId` 去重。
3. 校验与执行：
   - 根据事件类型路由到 Binding、Message、Action 或 Receipt Service。
   - 用户动作必须匹配有效的 `ImBinding`、目标 `Delivery` 和 `operationId`。
4. 返回结果：执行“知道了”“推迟”等业务动作，更新卡片或返回 H5 结果，并记录 `acted` 类型的 `DeliveryReceipt`。

### 流程四：平台回执与投递状态更新

此流程用于把不同平台的发送状态统一更新到系统投递记录。
1. 接收回执：Adapter 将平台发送结果转换为 `delivery.updated` 类型的 `NormalizedImEvent`。
2. 关联投递：通过平台消息 ID 或 `correlationId` 找到对应的 `Delivery` 和 `DeliveryAttempt`。
3. 更新状态：
   - 将平台结果映射为 `accepted`、`delivered`、`read`、`acted` 或 `failed` 的 `DeliveryReceipt`。
   - Receipt Service 幂等更新 `Delivery`，且不允许乱序回执造成状态倒退。

---

 