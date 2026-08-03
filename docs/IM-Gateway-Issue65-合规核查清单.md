# IM Gateway 骨架 · Issue #65 合规核查清单

> 依据：[Issue #65（产出模块数据模型和接口-20260723）](https://github.com/1024XEngineer/XE6-15/issues/65) 内 ZhaoXingPeng 的架构设计评论（Proposal-Accepted）
> 核查对象：`services/im-gateway`
> 修复后复核日期：2026-08-03（已重新拉取 Issue 定稿评论逐字复核）
> 状态图例：✅ 满足 ｜ 🟡 部分满足 ｜ ❌ 缺失/偏离

## 结论

`services/im-gateway` 的空骨架已完成 Issue #65 中 IM 模块的契约、应用边界、端口、模型、跨部署 HTTP/SSE 接口和进程内应用方法对齐。本清单 39 项均复核为✅；重新拉取原文后发现的 SSE 持久化回放、H5 Token 签发时机、`externalMessageId` 回执归并、标准事件命名等隐含差异亦已补齐。平台 SDK、数据库和网络服务器仍为 Port 或 mock，符合“只要空骨架代码”的范围。

## 范围说明

Issue #65 覆盖六个模块；当前只复核 IM Gateway。Voice / MCP / Schedule / TimingTask 以及真实 Koishi/微信适配实现不在本次代码骨架范围内。

## 一、跨端契约（P0）

| # | 复核结果 | 代码证据 | 状态 |
| --- | --- | --- | --- |
| C-01 | 四份跨端契约均已真实定义 | `contracts/device-gateway.ts` | ✅ |
| C-02 | strong 响应返回 `actionStream{reminderTriggerId,expiresAt}`，weak 不返回 | `contracts/device-gateway.ts:114`，`application/services.ts:473` | ✅ |
| C-03 | 动作结果支持 `nextTriggerAt?` | `contracts/device-gateway.ts:145` | ✅ |
| C-04 | `ReminderType = weak | strong`，SSE 只接受 strong | `contracts/device-gateway.ts:38`，`infrastructure/http/device-api.ts:182` | ✅ |
| C-05 | SSE 仅下行 Command，HTTP 层输出 `id/event=reminder.action/data`；Result 仅通过 HTTPS 回传 | `ports/external.ts`，`infrastructure/http/device-api.ts` | ✅ |
| C-06 | Command 包含 command/operation/trigger/expiry，snooze 使用 `params.minutes` | `contracts/device-gateway.ts` | ✅ |
| C-07 | SSE 支持 `Last-Event-ID`；先按 device + trigger + expiry 从 Action Repository 回放，再接实时流；deadline 由服务端 Delivery 解析 | `infrastructure/http/device-api.ts`，`application/services.ts` | ✅ |

## 二、幂等（P0）

| # | 复核结果 | 代码证据 | 状态 |
| --- | --- | --- | --- |
| I-01 | 通知以 `businessEventId` 去重 | `application/services.ts` 的通知提交流程 | ✅ |
| I-02 | 入站事件使用 `channelAccountId + externalEventId` 复合键 | `contracts/platform-events.ts`，`ports/repositories.ts`，`infrastructure/persistence/in-memory.ts` | ✅ |
| I-03 | 回执使用唯一 `dedupeKey`，先以 channel + `externalMessageId` 找 Delivery，并实行单调状态流转 | `application/services.ts`，`ports/repositories.ts` | ✅ |
| I-04 | attempt 使用 `deliveryId + attemptNo`，并包含唯一 requestId | `ports/repositories.ts:88`，`application/services.ts:694` | ✅ |
| I-05 | 业务动作使用 `operationId`，平台 action key 另以 hash 去重 | `application/services.ts`，`ports/repositories.ts` | ✅ |
| I-06 | SSE Command/Result 使用 Action ID 作为 `commandId` | `application/services.ts` 的 `toCommand`/`recordResult` | ✅ |

## 三、状态机（P0/P1）

| # | 复核结果 | 代码证据 | 状态 |
| --- | --- | --- | --- |
| S-01 | Delivery 具备全状态及重试/死信入口，dispatch 仅允许 pending/retryable_failed | `domain/models.ts`，`application/services.ts` | ✅ |
| S-02 | Action 具备全状态；retryable result 回 pending；`expireDue()` 落 expired 并关闭流 | `domain/models.ts`，`application/services.ts` | ✅ |
| S-03 | Receipt 状态仅 `delivered | failed` | `domain/models.ts:158` | ✅ |
| S-04 | delivered 不会被晚到 failed 回执覆盖 | `application/services.ts` 的 `advanceDeliveryStatus`，`app/mock-scenario.ts:81` | ✅ |

## 四、数据模型（§8.2）

| # | 复核结果 | 代码证据 | 状态 |
| --- | --- | --- | --- |
| D-01 | ExternalIdentity 仅保存 ciphertext/hash/status，明文仅在 infrastructure 边界短暂出现 | `domain/models.ts:66`，`ports/external.ts:51` | ✅ |
| D-02 | PairingSession 模型、Repository 和用例完整，`userId` 可空，确认时必须解析用户，过期扫描落 `expired` | `domain/models.ts`，`application/api.ts`，`application/services.ts` | ✅ |
| D-03 | Binding 支持 nullable deviceId、priority、unbound/revoked | `domain/models.ts:84` | ✅ |
| D-04 | Action 含 key hash、expected/actual identity、dispatch time、result | `domain/models.ts:174` | ✅ |
| D-05 | ChannelAccount 含 tenant/credential/connection/capability/error 字段 | `domain/models.ts:41` | ✅ |
| D-06 | Delivery 含 presentation/external message/expiry 字段 | `domain/models.ts:120` | ✅ |
| D-07 | Receipt 含 unique dedupeKey 与 attemptId | `domain/models.ts:154`，`ports/repositories.ts:92` | ✅ |
| D-08 | Attempt 含 renderedPayload 与 requestId | `domain/models.ts:137` | ✅ |

## 五、接口面（§6.1）

| # | 复核结果 | 代码证据 | 状态 |
| --- | --- | --- | --- |
| A-01 | Result 路由带 deviceId/commandId，设备主体与 path 一致；body 按 Issue 不重复内部 device/command/correlation 字段 | `infrastructure/http/device-api.ts`，`contracts/device-gateway.ts` | ✅ |
| A-02 | H5 GET/POST 仅接收 `{token,action,params?}`；Token 在 Delivery 渲染前签发，服务端解析身份 | `infrastructure/http/action-ui-api.ts`，`application/services.ts`，`ports/external.ts` | ✅ |
| A-03 | Channel account health 应用方法存在；当前同进程调用，不预留管理 HTTP | `application/api.ts`，`application/services.ts` | ✅ |
| A-04 | Binding 列表与解绑应用方法存在；当前同进程调用 | `application/api.ts`，`application/services.ts` | ✅ |
| A-05 | Delivery 详情与死信重试应用方法存在；当前同进程调用 | `application/api.ts`，`application/services.ts` | ✅ |
| A-06 | PairingSession POST/GET 接口存在，属设备侧，创建/查询均校验设备 Token 归属 | `infrastructure/http/device-api.ts` | ✅ |
| A-07 | 通知和日程回执业务接口存在 | `infrastructure/http/device-api.ts:32` | ✅ |
| A-08 | 内部标准事件精确使用 message.received/action.triggered/delivery.updated/binding.requested；Koishi 直接调用 `PlatformEventApplication`，入站状态可转 processing/processed/failed | `contracts/platform-events.ts`，`application/api.ts`，`application/services.ts` | ✅ |
| A-09 | 设备 Token 绑定 deviceId；Command/Result 校验 trigger、identity、expiry；SSE 不信任客户端 expiresAt | `infrastructure/http/device-api.ts`，`application/services.ts` | ✅ |

## 六、命名/形态对齐（P1）

| # | 复核结果 | 代码证据 | 状态 |
| --- | --- | --- | --- |
| N-01 | Platform 枚举精确为 wechat_official/wecom_aibot/feishu/dingtalk | `contracts/platform-events.ts:15` | ✅ |
| N-02 | Reminder Action 为 acknowledge/snooze，并定义通用 ActionIntent 的 bind_confirm/bind_cancel/open_url | `contracts/device-gateway.ts` | ✅ |
| N-03 | H5 Token claims 含 actionId/deliveryId/expiresAt | `ports/external.ts:136` | ✅ |
| N-04 | 通知对齐 reminder_due/recipient/content/command actions，通知、H5 与 SSE 共用 `params` 形态 | `contracts/device-gateway.ts` | ✅ |
| N-05 | 业务层仅据 ChannelCapabilities 选能力，无平台名分支 | `domain/models.ts:27`，`ports/external.ts:65`，`application/services.ts:1229` | ✅ |

## 自动复核

```bash
tsc -p services/im-gateway/tsconfig.json --noEmit
tsc -p services/im-gateway/tsconfig.json \
  --outDir /tmp/voicelife-im-gateway-issue65-final \
  --declaration false --declarationMap false --sourceMap false
node --input-type=module -e \
  "const m=await import('/tmp/voicelife-im-gateway-issue65-final/app/mock-scenario.js'); await m.runMockNotificationScenario()"
```

修复后场景覆盖：强/弱提醒、DeliveryAttempt 与稳定 H5 Token 渲染、`externalMessageId` 回执归并与状态不倒退、复合入站幂等、H5/标准平台动作入口合流、SSE 持久化回放、retryable result 不确认命令、HTTPS 终态结果、Action 过期关闭、PairingSession 可空用户与过期状态。
