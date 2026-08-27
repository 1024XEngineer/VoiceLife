# VoiceLife IM Gateway

这是 VoiceLife IM Gateway 的独立服务模块。它用代码表达模块边界、跨端契约和依赖方向，并以 Issue #95 作为当前交付与验收基线；目前包含 PostgreSQL 持久化、真实 Koishi Core Runtime、实时 SSE Hub、微信公众号 Webhook/模板投递 Adapter、服务端渲染的 H5 Action UI，以及可直接部署的生产 HTTP/SSE 进程。

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

## 验证

仓库已安装 TypeScript 时执行：

```bash
pnpm --dir services/im-gateway check
pnpm --dir services/im-gateway build
pnpm --dir services/im-gateway test
```

## 生产进程与部署

`pnpm start` 从仓库根目录 `.env` 解析配置，自动执行 PostgreSQL migration，装配真实
`PostgresImUnitOfWork`、Koishi Runtime、SSE Hub 与 `WechatOfficialAdapter`，然后监听设备 API、Action UI、
`/wechat` 和 `/healthz`。生产进程不使用 `createMockImGateway()`。

复制 [`.env.example`](../../.env.example) 后填入部署值；其中的 `replace-me` 会被生产配置故意拒绝，不能直接启动。
生产进程不再读取单例 `DEVICE_ID`、`DEVICE_USER_ID` 或 `DEVICE_TOKEN`。设备必须先通过下述 CLI 注册，Gateway
只按数据库中的 SHA-256 摘要认证 43 字符 base64url Token。`ACTION_TOKEN_SECRET` 至少 32 字节；建议额外提供
独立的 `IDENTITY_SECRET`。结构化日志不会记录 Authorization、请求体、Action token、动态 URL 或 Secret。

配对 DTO 的隐私收紧必须先部署 Gateway，再发布依赖该 DTO 的固件；回滚时也应先停止固件放量。发布探针应确认
`POST /v1/im/pairing-sessions` 的 `session` 只含公开字段且没有 `displayCodeHash`/外部身份，再开放 USB 配对验证。

本机先启动 PostgreSQL，再启动进程：

```bash
docker compose up -d postgres
pnpm --dir services/im-gateway start
curl http://127.0.0.1:3000/healthz
```

完整容器部署要求 `.env` 额外提供 `POSTGRES_PASSWORD`，然后执行：

```bash
docker compose up --build -d
docker compose ps
```

容器内保留 `.env` 的 `DATABASE_URL` 凭据与数据库名，只由 Compose 注入的 `DATABASE_HOST=postgres` 替换
宿主机地址；`DATABASE_URL` 中的密码必须与 `POSTGRES_PASSWORD` 一致。Gateway 在 PostgreSQL healthcheck
通过后启动，自身 healthcheck 会持续探测数据库中的渠道账号与 Koishi Bot 运行状态。Compose 默认只把
Gateway 端口绑定到宿主机 loopback，避免设备 API 绕过公网 HTTPS 入口。

监听器使用 HTTP，公网 HTTPS 必须由宿主机上的 Cloudflare Tunnel 或反向代理终止 TLS。Quick Tunnel 联调可先运行：

```bash
cloudflared tunnel --url http://127.0.0.1:3000
```

把生成的 HTTPS 域名写入 `WECHAT_ACTION_UI_BASE_URL`，其路径必须以
`/voicelife/reminder-actions` 结尾；微信后台 webhook URL 使用同一域名的 `/wechat`。正式环境应使用具名
Tunnel 或服务器反向代理，以免 Quick Tunnel 重启后域名改变。

生产监听路由包括：

- `POST /v1/im/pairing-sessions` 与 `GET /v1/im/pairing-sessions/:pairingSessionId`
- `POST /v1/im/schedule-receipts`、`POST /v1/im/schedule-query-results` 与 `POST /v1/im/notifications`
- `GET /v1/devices/:deviceId/reminder-actions/stream`（SSE）
- `POST /v1/devices/:deviceId/reminder-actions/:commandId/result`
- `GET|POST /voicelife/reminder-actions/:token`
- `GET /voicelife/reminder-actions/query-result/:token`
- `GET|POST /wechat` 与 `GET /healthz`

日程查询模板消息会附带一条 7 天有效的只读 H5 链接，路径为
`WECHAT_ACTION_UI_BASE_URL/query-result/<token>`。页面仅从已持久化的查询结果读取数据，不包含 JSON 载荷、表单或写入接口，
并复用 `WECHAT_ACTION_UI_BASE_URL`，无需新增环境变量。

通知受理与 Delivery Outbox 事件在同一 PostgreSQL 事务内提交；常驻 worker 领取事件后派发，并在启动时恢复
`pending`、`retryable_failed` 与租约已过期的 `sending` Delivery。临时失败按 `availableAt` 延迟重试，HTTP
进程只负责在事务提交后唤醒 worker，不再承担可能随 202 响应丢失的进程内派发任务。结构化日志用同一个
`correlationId` 记录设备受理、Delivery 派发、Action 提交与 SSE 下发；异步失败只记录稳定错误码，不回显
凭据或平台载荷。自动投递最多尝试五次并按 1/2/4/8 分钟退避，耗尽或遇到永久失败后进入死信；目标绑定在
受理后失效也会保存失败 Attempt，不会留下无法恢复的 `pending` Delivery。

生产微信公众号 Bot 注册在真实 Koishi Context 中；Delivery 通过 `KoishiChannelAdapter` 选择并校验 Bot 后再
调用微信传输。SSE Hub 同时限制总连接、单设备、单提醒窗口和慢客户端队列，溢出连接依赖 Action Repository
在重连时回放，HTTP 序列化会等待 socket 背压解除。

跨端 JSON fixture 位于 `contracts/im-gateway/v1/fixtures`，由 C++ 主机测试与 TypeScript 测试共同消费。

## 设备注册、轮换与吊销

每台设备使用独立 Token；同一用户可拥有多台设备。所有命令都会先迁移数据库：

```bash
docker compose exec gateway pnpm device -- create --user-id user-1
docker compose exec gateway pnpm device -- list --user-id user-1
docker compose exec gateway pnpm device -- rotate-token --device-id <deviceId>
docker compose exec gateway pnpm device -- revoke --device-id <deviceId>
```

`create`/`rotate-token` 只在 stdout 显示一次 43 字符 Token；数据库只保存 32 字节 SHA-256 摘要，丢失后只能轮换。
`revoke` 幂等取消全部 pending 配对会话，但保留绑定、投递和回执历史。退出码固定为 0 成功、2 参数错误、3 不存在、
4 重复或状态冲突、5 数据库/内部错误。

服务器签发后，通过受控 USB 写入设备（Token 由隐藏提示读取，不进入 shell 参数）：

```bash
python3 scripts/provision_im_config.py --port /dev/ttyUSB0 \
  --gateway-origin https://gateway.example.com --device-id <deviceId> --user-id <userId>
```

覆盖或凭据暴露时先轮换，再使用 `--force` 写入。仅清理/重配 Wi-Fi 不得清除 IM NVS；完整安全擦除是单独操作。
交付前的真实设备认证冒烟必须核对订单中的不可变身份，并等待一分钟会话自然过期：

```bash
python3 scripts/start_im_pairing.py --port /dev/ttyUSB0 --auth-smoke \
  --expected-device-id <deviceId> --expected-user-id <userId>
```

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
进程退出或优雅停机前调用 `close()` 释放连接池。`ports` 需替换为平台 Capability、设备认证和 Secret
实现；Koishi 与 SSE 的同进程装配由下述 Runtime 组合根完成。

当前 mock 场景覆盖：PairingSession 绑定/过期、强弱提醒分流、DeliveryAttempt 与 H5 Token 渲染、复合入站幂等键、`externalMessageId` 回执归并、Receipt 去重及迟到回执不倒退、H5/平台 Action 入口合流、SSE 持久化回放、HTTPS Result 回传与 Action 过期关闭。

## Koishi Runtime 与 SSE Hub

`createKoishiGatewayRuntime()` 使用真实 `@koishijs/core` `Context`，将 `VoiceLifeKoishiPlugin`、
`KoishiChannelAdapter`、`SseActionCommandHub` 与 Gateway Application 装配在同一进程。`start()` 启动
Koishi 生命周期，`close()` 注销插件监听并停止 Context；重复调用不会重复启动或释放。

`KoishiChannelAdapter` 在发送时从 `ChannelAccount` 解析 `koishiBotId`，只允许同账号私聊，并通过部署层
注入的身份解密函数取得短时平台用户标识。`KoishiContextBotFacade` 从真实 Context 选择 active Bot，调用
`sendPrivateMessage()` 并把首个平台消息 ID 返回给 DeliveryAttempt。平台 Bot 不存在或离线时返回可重试的
`koishi_bot_unavailable`，不会伪造平台受理。

`VoiceLifeKoishiPlugin` 直接监听 Koishi 的 `message` 与 `interaction/button` 事件，平台 Capability 在
Infrastructure 内完成归一化后直接调用 `PlatformEventApplication`，中间不经过内部管理 HTTP。

`SseActionCommandHub` 为设备的实时 SSE 连接按 `deviceId + reminderTriggerId` 隔离命令，并在 Action 完成、
订阅取消或窗口过期时关闭。Hub 不把 `Last-Event-ID` 当作业务 ACK，也不承担持久化；断线和进程重启后的
未确认命令始终从 Action Repository 回放。#152 的公开监听进程负责把该异步流序列化为真正的 SSE 响应，
并注入平台 Bot、PostgreSQL、Secret 与健康检查。

## 微信公众号 Adapter 与 Webhook

`WechatOfficialAdapter` 按渠道账号实例化，构造时必须接收 `channelAccountId`、公众号原始 ID `expectedToUserName` 和由部署环境解析后的微信 Token。真实 Token 不写入 `ChannelAccount.capabilityConfig`、Profile、日志或测试 fixture；测试仅使用无效固定值。`verifyWebhook()` 处理服务器配置的 `echostr` 验签和五分钟重放窗口，`normalizeInbound()` 校验 POST 签名、`ToUserName` 账号归属并将明文模式的微信 XML 转换为 `NormalizedImEvent`。

生产组合根通过 `wechatAdapter` 注入 Adapter 后暴露 `runtime.wechatApi`，HTTP 框架将微信 GET/POST 请求映射到 `WechatWebhookController.verify()`/`post()`；框架仍必须在读取请求体前配置 64 KiB 流式限制。当前不支持微信 Webhook AES 加密模式。

配置 `outbound` 后，同一个 `WechatOfficialAdapter` 同时实现 `ChannelCapabilityResolver`、`DeliveryRendererPort` 和 `ImChannelPort`。组合根应把同一实例注入 `channelCapabilities`、`deliveryRenderer`、`imChannel` 与 `wechatAdapter`。Adapter 获取并缓存 `access_token`，通过微信模板消息接口发送通知；强提醒模板的详情地址只携带 URL 编码后的动作 token。模板接口成功只记录 `accepted` 和精确字符串 `msgid`，后续 `TEMPLATESENDJOBFINISH` 回调才把 Delivery 推进为 `delivered`。

`WECHAT_DISPLAY_TIME_ZONE` 使用 IANA 时区名（默认 `Asia/Shanghai`），模板中的 UTC 业务时间会格式化为
`2026年8月10日 16:42` 这类用户可读文本。用户发送“帮助”或未识别的文本时，Webhook 会同步返回微信被动文本 XML，
提示其以 `绑定 123456` 的格式发送六码绑定码；有效绑定会同步返回成功提示，无效或过期的绑定码会返回重新获取提示。
这些被动回复均不依赖模板消息权限。

提醒和日程变更回执使用 `WECHAT_TEMPLATE_*` 配置的模板；日程查询结果使用独立的
`WECHAT_QUERY_TEMPLATE_ID`、`WECHAT_QUERY_TEMPLATE_TITLE_FIELD`、`WECHAT_QUERY_TEMPLATE_BODY_FIELD` 与
`WECHAT_QUERY_TEMPLATE_TIME_FIELD`。两套模板 ID 必须不同，每套模板的三个字段也必须互不相同。

新的 pending 会话再次确认完全相同组合时复用已有有效绑定。设备改绑新身份或身份改绑新设备时，旧关系会保留为
`unbound` 历史；任一设备和任一外部身份最多各有一条 active 绑定。设备吊销不会删除绑定或投递历史。

`ChannelAccount.credentialRef` 只保存 `secret://...` 引用。部署层负责解析并注入 Webhook Token、App ID/AppSecret、模板 ID/字段映射、H5 HTTPS 基础地址以及外部身份解密函数；这些值不得写入 `capabilityConfig`、Profile、日志或 fixture。未配置 `outbound` 时 Adapter 继续只提供入站能力，并如实返回 `proactiveMessage: false`。

模板投递结果使用 `channelAccountId + MsgID` 定位 Delivery；`MsgID + Status` 生成稳定的 webhook 事件标识和 Receipt 去重键。重复回调由入站事件与 Receipt 两层幂等保护，迟到回执继续遵循 Application 层状态机，不会让已投递状态倒退。

### 微信测试号联调 harness

`dev:wechat` 是微信 Adapter 的本地联调入口，不是生产启动进程。它与 Gateway 共用 PostgreSQL、设备注册表、
数据库认证和一对一绑定语义，仅在 loopback 地址暴露微信 Webhook、H5 Action UI、健康检查以及 Bearer 保护的
测试投递/状态查询端点。

先运行 `pnpm --dir services/im-gateway device -- create --user-id <userId>` 注册测试设备，安全保存一次性输出的
Token，并把非 Secret 的 deviceId 写入 `WECHAT_DEV_DEVICE_ID`，把测试用户 OpenID 写入
`WECHAT_TEST_OPENID`。`.env` 还需 `DATABASE_URL`；不要把 Token、OpenID、AppSecret 或 `.env` 提交到仓库或粘贴到日志。

由于 Quick Tunnel 域名在启动后才生成，可以先启动 Tunnel，再把其 HTTPS 域名写入 `.env`：

```bash
cloudflared tunnel --url http://127.0.0.1:3000
```

```dotenv
WECHAT_DEV_HOST=127.0.0.1
WECHAT_DEV_PORT=3000
WECHAT_DEV_DEVICE_ID=<registered-device-id>
WECHAT_TEST_OPENID=<test-user-openid>
WECHAT_WEBHOOK_PUBLIC_URL=https://example.trycloudflare.com/wechat
WECHAT_ACTION_UI_BASE_URL=https://example.trycloudflare.com/voicelife/reminder-actions
```

保持 Tunnel 运行，在另一个终端启动 harness：

```bash
pnpm --dir services/im-gateway run dev:wechat
curl https://example.trycloudflare.com/healthz
```

微信测试号后台的服务器 URL 使用 `WECHAT_WEBHOOK_PUBLIC_URL`，Token 必须与本地
`WECHAT_WEBHOOK_TOKEN` 完全一致，并选择明文模式。验证成功后，从本机触发一次真实投递：

```bash
curl -X POST \
  -H 'Authorization: Bearer <DEVICE_TOKEN>' \
  http://127.0.0.1:3000/__dev/wechat/send-test
```

响应中的 `deliveryId` 可用于检查平台回执是否把状态从 `accepted` 推进为 `delivered`：

```bash
curl -H 'Authorization: Bearer <DEVICE_TOKEN>' \
  http://127.0.0.1:3000/__dev/wechat/deliveries/<deliveryId>
```

## H5 Action UI

生产运行时同时暴露 `runtime.actionUiPageApi`。HTTP 框架将 `GET /voicelife/reminder-actions/:token` 映射到 `get()`，将表单 POST 映射到 `post()`，并原样写回状态码、响应头和 HTML body。页面不运行客户端脚本，只渲染通知意图中服务端批准的动作标签与固定参数；路径 token 由服务端覆盖请求体中的同名字段，浏览器看不到内部身份、Delivery、Action 或 Operation 标识。

生产部署使用 `AesGcmActionTokenPort`。它以部署 Secret 派生 AES-256-GCM 密钥，令牌内容不可读且可在进程重启后校验；应用层继续检查动作窗口、绑定和 token + 动作幂等。Secret 必须由安全引用解析后注入，不能使用示例值或持久化到数据库。

## TSDoc 规范

所有导出的 class、interface、type、enum、const 和 function 都必须紧邻简洁的 `/** ... */` TSDoc 注释，并包含中文职责说明。导出函数、导出接口的方法，以及导出类的公开构造函数、方法和访问器还必须逐一使用 `@param` 说明参数，并通过 `@returns` 说明非 `void` 返回值。实现类可使用 `{@inheritDoc Interface.method}` 继承接口契约；`private` 和 `protected` 成员不强制添加重复代码的注释。`pnpm run docs:check` 每次全量扫描 `src`，不区分新旧代码。
