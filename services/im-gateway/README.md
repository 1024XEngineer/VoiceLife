# VoiceLife IM Gateway

这是 VoiceLife IM Gateway 的独立服务模块。它用代码表达模块边界、跨端契约和依赖方向，并以 Issue #95 作为当前交付与验收基线；目前包含 PostgreSQL 持久化、微信公众号 Webhook/模板投递 Adapter 和服务端渲染的 H5 Action UI，但仍不包含真实 Koishi Bot。

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
├── infrastructure/  # 设备/Action UI HTTP、Koishi、微信公众号和持久化适配器
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

## PostgreSQL 持久化

持久化契约测试（`test/persistence-contract.test.mjs`）会用同一套断言分别跑内存实现与
`PostgresImUnitOfWork`，覆盖投递、尝试、回执、用户动作和事务性发件箱的跨聚合事务。

本地用 Docker 启动 PostgreSQL：

```bash
docker compose up -d postgres
```

连接参数默认取 `postgres://voicelife:voicelife@localhost:5432/voicelife`，可通过环境变量覆盖：

```bash
DATABASE_URL=postgres://voicelife:voicelife@localhost:5432/voicelife \
  node --test test/persistence-contract.test.mjs
```

PostgreSQL 不可用时对应测试自动跳过，其余断言照常执行；CI 通过 service container 提供相同的
PostgreSQL 16，确保契约套件在真实数据库上通过。

`createMockImGateway()` 使用内存 Repository 和 Mock 通道，可用于测试与本地串联。生产装配使用
`createPostgresImGateway({ databaseUrl?, ports })`：连接地址优先取入参，其次 `DATABASE_URL` 环境变量，
缺省回落本地 docker-compose 地址；组合根自动执行 schema 迁移并托管连接池，返回 `{ runtime, close() }`，
进程退出或优雅停机前调用 `close()` 释放连接池。`ports` 需替换为 Koishi、微信 Capability Plugin 和真实
SSE Hub 等实现。

当前 mock 场景覆盖：PairingSession 绑定/过期、强弱提醒分流、DeliveryAttempt 与 H5 Token 渲染、复合入站幂等键、`externalMessageId` 回执归并、Receipt 去重及迟到回执不倒退、H5/平台 Action 入口合流、SSE 持久化回放、HTTPS Result 回传与 Action 过期关闭。

## 微信公众号 Adapter

`WechatOfficialAdapter` 按渠道账号实例化，构造时必须接收 `channelAccountId`、公众号原始 ID `expectedToUserName` 和由部署环境解析后的微信 Token。真实 Token 不写入 `ChannelAccount.capabilityConfig`、Profile、日志或测试 fixture；测试仅使用无效固定值。`verifyWebhook()` 处理服务器配置的 `echostr` 验签和五分钟重放窗口，`normalizeInbound()` 校验 POST 签名、`ToUserName` 账号归属并将明文模式的微信 XML 转换为 `NormalizedImEvent`。

生产组合根通过 `wechatAdapter` 注入 Adapter 后暴露 `runtime.wechatApi`，HTTP 框架将微信 GET/POST 请求映射到 `WechatWebhookController.verify()`/`post()`；框架仍必须在读取请求体前配置 64 KiB 流式限制。当前不支持微信 Webhook AES 加密模式。

配置 `outbound` 后，同一个 `WechatOfficialAdapter` 同时实现 `ChannelCapabilityResolver`、`DeliveryRendererPort` 和 `ImChannelPort`。组合根应把同一实例注入 `channelCapabilities`、`deliveryRenderer`、`imChannel` 与 `wechatAdapter`。Adapter 获取并缓存 `access_token`，通过微信模板消息接口发送通知；强提醒模板的详情地址只携带 URL 编码后的动作 token。模板接口成功只记录 `accepted` 和精确字符串 `msgid`，后续 `TEMPLATESENDJOBFINISH` 回调才把 Delivery 推进为 `delivered`。

`ChannelAccount.credentialRef` 只保存 `secret://...` 引用。部署层负责解析并注入 Webhook Token、App ID/AppSecret、模板 ID/字段映射、H5 HTTPS 基础地址以及外部身份解密函数；这些值不得写入 `capabilityConfig`、Profile、日志或 fixture。未配置 `outbound` 时 Adapter 继续只提供入站能力，并如实返回 `proactiveMessage: false`。

模板投递结果使用 `channelAccountId + MsgID` 定位 Delivery；`MsgID + Status` 生成稳定的 webhook 事件标识和 Receipt 去重键。重复回调由入站事件与 Receipt 两层幂等保护，迟到回执继续遵循 Application 层状态机，不会让已投递状态倒退。

## H5 Action UI

生产运行时同时暴露 `runtime.actionUiPageApi`。HTTP 框架将 `GET /voicelife/reminder-actions/:token` 映射到 `get()`，将表单 POST 映射到 `post()`，并原样写回状态码、响应头和 HTML body。页面不运行客户端脚本，只渲染通知意图中服务端批准的动作标签与固定参数；路径 token 由服务端覆盖请求体中的同名字段，浏览器看不到内部身份、Delivery、Action 或 Operation 标识。

生产部署使用 `AesGcmActionTokenPort`。它以部署 Secret 派生 AES-256-GCM 密钥，令牌内容不可读且可在进程重启后校验；应用层继续检查动作窗口、绑定和 token + 动作幂等。Secret 必须由安全引用解析后注入，不能使用示例值或持久化到数据库。

## TSDoc 规范

所有导出的 class、interface、type、enum、const 和 function 都必须紧邻简洁的 `/** ... */` TSDoc 注释，并包含中文职责说明。导出函数、导出接口的方法，以及导出类的公开构造函数、方法和访问器还必须逐一使用 `@param` 说明参数，并通过 `@returns` 说明非 `void` 返回值。实现类可使用 `{@inheritDoc Interface.method}` 继承接口契约；`private` 和 `protected` 成员不强制添加重复代码的注释。`pnpm run docs:check` 每次全量扫描 `src`，不区分新旧代码。
