# VoiceLife IM Gateway skeleton

这是 VoiceLife IM 模块的可编译空骨架。它用代码表达模块边界、跨端契约和依赖方向，并以 Issue #95 作为当前交付与验收基线；本目录不包含生产数据库、真实 Koishi Bot 或微信验签实现。

## 边界

- ESP32 本地的 Schedule、TimerTask、TimerInstance、ReminderRule 和 ReminderTrigger 是业务事实源。
- IM Gateway 只拥有外部身份、绑定、投递、平台回执、用户动作和 IM Outbox。
- IM 用户动作经校验后生成 `ReminderActionCommand`，不能直接修改设备端事实。
- Koishi、微信和数据库类型不得进入 `domain` 或 `application`。
- 强提醒返回有过期时间的 ActionStream；弱提醒不建立 SSE。
- SSE 按 `deviceId + reminderTriggerId` 隔离，结果只通过 HTTPS 回传。
- SSE 重连先从 Action Repository 查询未确认命令，`Last-Event-ID` 不代替业务 ACK。
- H5 只提交 `{token, action, params?}`，内部身份、Delivery 和动作标识均由服务端解析。
- 平台回执以 `channelAccountId + externalMessageId` 定位 Delivery，不要求平台知道内部 `deliveryId`。
- DeliveryAttempt、DeliveryReceipt、Action 分别记录平台受理、投递证据和用户动作。
- Koishi Plugin、Handler 与 IM Application 同进程组合，通过 Application/Port 直接调用，不保留内部管理 HTTP 接口。
- HTTP/SSE Controller 只承载设备侧与 Action UI 的真实跨部署边界。
- HTTP JSON 在进入 Application 前按 `schemaVersion`、字段、枚举和 ISO 8601 时间完成运行时校验。
- 请求级幂等记录同时保存规范化指纹和原始响应；相同事件 ID 的异内容重放会明确冲突。

## 目录

```text
src/
├── contracts/       # ESP32 ↔ Gateway 与平台无关 DTO
├── domain/          # IM 领域模型
├── application/     # 入站用例接口与应用服务
├── ports/           # Repository、通道、动作流、时钟等 Port
├── infrastructure/  # 设备/Action UI HTTP、Koishi、微信和持久化适配器桩
├── app/             # 组合根
└── index.ts          # 公共导出
```

## 骨架验证

仓库已安装 TypeScript 时执行：

```bash
pnpm --dir services/im-gateway check
pnpm --dir services/im-gateway build
pnpm --dir services/im-gateway test
```

跨端 JSON fixture 位于 `contracts/im-gateway/v1/fixtures`，由 C++ 主机测试与 TypeScript 测试共同消费。

`createMockImGateway()` 使用内存 Repository 和 Mock 通道，可用于后续主干串联测试。生产装配应替换为 PostgreSQL、Koishi、微信 Capability Plugin 和真实 SSE Hub。

当前 mock 场景覆盖：PairingSession 绑定/过期、强弱提醒分流、DeliveryAttempt 与 H5 Token 渲染、复合入站幂等键、`externalMessageId` 回执归并、Receipt 去重及迟到回执不倒退、H5/平台 Action 入口合流、SSE 持久化回放、HTTPS Result 回传与 Action 过期关闭。
