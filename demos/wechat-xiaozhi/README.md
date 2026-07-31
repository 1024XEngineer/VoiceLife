# VoiceLife Koishi + Satori × 微信公众号 × 小智 Demo

这是一个使用 Koishi 作为 IM 运行时、Satori 作为统一协议出口的验证服务：

```text
微信公众号渠道
├─ 微信公众号 IM → WeChat Adapter → binding.requested → Binding Handler
└─ Action UI（H5）→ plugin-server → Action Route ───────────────┐
未来原生卡片 → Platform Adapter → interaction/button ──────────┤
                                                            ↓
                                              ReminderActionHandler
                                                            ↓
                                                IM Application.Action
                                                            ↓
                                                Reminder Command Port

未来其他 IM → Koishi Adapter → VoiceLife 业务插件
Koishi Runtime → 可选 Satori HTTP / WebSocket

小智语音 → MCP 工具 → VoiceLife 提醒 → 微信公众号模板消息
                                          ↓
                 点击消息 → Action UI → 统一动作入口 → VoiceLife 状态
```

微信公众号只有一个 Koishi Adapter。H5 是同一渠道的 Action UI 补充，未来可以换成小程序，但不会因此增加第二个微信公众号 Adapter。

已实现：

- Koishi 官方微信公众号适配器和标准 Session；
- Satori HTTP / WebSocket 协议服务；
- 旧 `/wechat/callback` 到 Koishi `/wechat-official` 的迁移兼容入口；
- 接收文字、语音、关注、扫码、取消关注事件；
- 用短绑定码或带参数二维码绑定小智设备与公众号 OpenID；
- 定时提醒和模板消息发送；
- `TEMPLATESENDJOBFINISH` 发送完成事件；
- 模板消息跳转到带签名的 H5 快捷操作页；
- 网页一键“知道了”或“推迟 10 分钟”；
- 公众号文字消息仅用于设备绑定；
- 平台绑定输入统一规范化为 `ExternalIdentity` 和 `binding.requested`；
- H5 与原生卡片动作统一经过 `IM Application.Action` 和 `Reminder Command Port`；
- 小智 MCP 接入点 WebSocket 客户端；
- 生成绑定码、创建/查询/关闭/推迟提醒五种 MCP 工具；
- 本地 JSON 持久化、发送幂等和 Mock 模式。

## 1. 环境要求

- Node.js 22 或更高版本；
- 实体小智设备；
- 小智官方控制台或兼容 MCP 接入点的自建服务；
- 微信公众平台测试号，或者认证服务号；
- 真实微信联调时需要一个公网 HTTPS 地址。

安装依赖：

```bash
npm install
```

核心依赖为 Koishi 4.18、微信公众号适配器和 Satori Server。

## 2. 先跑本地闭环

```bash
cd demos/wechat-xiaozhi
cp .env.example .env
npm start
```

保持服务运行，在另一个终端执行：

```bash
cd demos/wechat-xiaozhi
npm run demo
```

模拟脚本会完成：

1. 为 `xiaozhi-demo-01` 生成绑定码；
2. 模拟公众号用户绑定；
3. 创建一条 1 秒后到期的提醒；
4. 调度器在 Mock 模式“发送”微信模板消息；
5. 打印绑定、提醒和 Outbox 状态。

浏览器可打开：

```text
http://localhost:8787/
http://localhost:8787/health
```

运行测试：

```bash
npm test
```

## 3. 接入手边的小智

### 3.1 获取 MCP 接入点

如果设备连接小智官方服务：

1. 登录 [xiaozhi.me](https://xiaozhi.me/)；
2. 选择手边的设备；
3. 进入“配置角色”；
4. 找到并复制 MCP 接入点的 `wss://...` 地址。

把地址和设备标识写入 `.env`：

```dotenv
XIAOZHI_MCP_ENDPOINT=wss://这里填写控制台复制的接入点
XIAOZHI_DEVICE_ID=xiaozhi-living-room
XIAOZHI_MCP_DEBUG=true
```

重启 `npm start`。日志出现以下内容表示外挂工具已连接：

```text
[xiaozhi] MCP 已连接，device=xiaozhi-living-room
```

调试模式只打印 MCP 方法名、关闭码和连接时长，不会输出接入点 Token。确认稳定后可将 `XIAOZHI_MCP_DEBUG` 改回 `false`。

正常连接日志应包含：

```text
[xiaozhi] ← initialize
[xiaozhi] ← notifications/initialized
[xiaozhi] ← tools/list
[xiaozhi] ← ping
```

`ping` 是小智 MCP 接入点的 JSON-RPC 保活请求，服务必须返回空结果 `{}`。如果连接成功后周期性断开，先开启调试模式，检查最后一个方法和关闭码；桥接器会使用指数退避重连，避免固定频率持续冲击接入点。

公众号消息进入 Koishi 后，服务端应出现：

```text
[I] voicelife wechat message user=...
```

如果微信里没有回复且服务端完全没有这两行，问题位于公网回调 URL、Tunnel、端口转发或发送的公众号账号，不在消息处理逻辑。

回到小智控制台刷新 MCP 工具，应看到：

- `voicelife.create_binding_code`
- `voicelife.create_reminder`
- `voicelife.list_reminders`
- `voicelife.dismiss_reminder`
- `voicelife.snooze_reminder`

如果使用自建 `xiaozhi-esp32-server`，在其控制台中取得兼容的 MCP 接入点即可，不需要修改本 Demo 的 HTTP 接口。

### 3.2 建议的小智角色提示词

把下面的规则追加到设备角色提示词：

```text
你是 VoiceLife 日程助手。
当用户要求创建提醒时，必须调用 voicelife.create_reminder，不要只在对话中口头答应。
调用时间参数必须使用带时区的 ISO 8601，中国时区使用 +08:00。
当用户要求绑定微信时，调用 voicelife.create_binding_code，并清晰、逐字符读出绑定码。
当用户查询、关闭或推迟提醒时，使用对应的 voicelife 工具。
```

### 3.3 设备侧验证语句

对小智说：

```text
帮我生成微信绑定码
```

小智应该读出类似 `9KF3QM` 的六位码。

绑定完成后再说：

```text
一分钟后提醒我喝水
```

服务日志和 `/api/state` 中应出现提醒。一分钟后调度器会发送微信消息；Mock 模式只记录发送，Live 模式调用真实模板接口。

## 4. 接入微信公众号

### 4.1 准备测试账号

可以先使用[微信公众平台接口测试号](https://mp.weixin.qq.com/debug/cgi-bin/sandbox?t=sandbox/login)，验证完成后再切换认证服务号。

需要取得：

- 公众号原始 ID（`gh_...`）；
- AppID；
- AppSecret；
- 自己设置的 Token；
- EncodingAESKey（如果启用安全模式）；
- 模板 ID 和模板字段名。

### 4.2 暴露公网 HTTPS

微信服务器必须能访问本机的：

```text
https://你的公网域名/wechat-official
```

可使用已有服务器反向代理，也可以在开发阶段使用可信的 HTTPS Tunnel。Tunnel 应转发到本机 `8787` 端口。

设置：

```dotenv
KOISHI_SELF_URL=https://你的公网域名
WECHAT_ACCOUNT=gh_公众号原始ID
WECHAT_TOKEN=与公众平台服务器配置完全一致的Token
```

公众平台“服务器配置”填写：

```text
URL:      https://你的公网域名/wechat-official
Token:    与 WECHAT_TOKEN 相同
消息加密: 初次验证可选明文；正式建议安全模式
```

安全模式还需要：

```dotenv
WECHAT_APP_ID=wx...
WECHAT_AES_KEY=公众平台的43字符EncodingAESKey
```

`/wechat/callback` 暂时保留为迁移兼容入口。未配置 `WECHAT_ACCOUNT` 时，
向旧入口发送一条公众号消息，服务端日志会打印公众号原始 ID；补入 `.env`
并重启后，再把公众平台 URL 切到 `/wechat-official`。

### 4.3 配置真实模板发送

在 `.env` 中设置：

```dotenv
WECHAT_OUTBOUND_MODE=live
WECHAT_APP_ID=wx...
WECHAT_APP_SECRET=...
WECHAT_TEMPLATE_ID=...
WECHAT_TEMPLATE_TITLE_FIELD=thing1
WECHAT_TEMPLATE_TIME_FIELD=time2
WECHAT_TEMPLATE_STATUS_FIELD=thing3
WECHAT_TEMPLATE_DETAIL_URL=https://你的公网域名/voicelife/reminder-actions
WECHAT_ACTION_TOKEN_SECRET=一段独立随机密钥
```

三个字段名必须根据实际模板内容调整，不能直接假设示例中的 `thing1/time2/thing3` 与真实模板一致。

模板示意：

```text
提醒事项：{{thing1.DATA}}
提醒时间：{{time2.DATA}}
当前状态：{{thing3.DATA}}
```

### 4.4 绑定并测试

1. 对小智说“帮我生成微信绑定码”；
2. 在公众号中发送：

   ```text
   绑定 9KF3QM
   ```

3. 公众号返回“绑定成功”；
4. 对小智说“一分钟后提醒我喝水”；
5. 到点后收到模板通知；
6. 点击模板消息进入操作页；
7. 点击“知道了”或“推迟 10 分钟”。

操作链接包含签名令牌，不暴露 OpenID。推迟后旧链接立即失效，再次到点会生成新链接。
公众号文字消息只处理“绑定”；提醒动作统一从 H5 或未来原生卡片进入。

也可以调用 API 生成带参数二维码：

```bash
curl -X POST http://localhost:8787/api/binding-codes \
  -H 'Authorization: Bearer change-me' \
  -H 'Content-Type: application/json' \
  -d '{"deviceId":"xiaozhi-living-room","withQr":true}'
```

用户扫描二维码后，公众号的 `subscribe` 或 `SCAN` 事件会自动完成绑定。

## 5. 回执验证

模板消息发送后，Demo 先记录：

```text
deliveryStatus = accepted
```

微信把 `TEMPLATESENDJOBFINISH` 事件推送到回调地址后，更新为微信返回的状态，例如：

```text
deliveryStatus = success
```

查看状态：

```bash
curl http://localhost:8787/api/state \
  -H 'Authorization: Bearer change-me'
```

公众号没有通用已读回执。用户点击 H5 的“知道了/推迟 10 分钟”，才是 VoiceLife 可确认的业务回执。

H5 和未来原生卡片不各自实现提醒逻辑。Koishi 插件中的 H5 Route 与
`interaction/button` Handler 都调用同一个 `ReminderActionHandler`；
该 Handler 校验令牌后调用 `IM Application.Action`，再经
`Reminder Command Port` 交给 Demo 中模拟本地 `TimingTask` 的业务实现。
卡片操作完成后优先通过 Koishi 通用 `bot.editMessage()` 更新原消息。

## 6. HTTP API

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/health` | 配置与健康状态 |
| GET/POST | `/wechat-official` | Koishi 官方微信适配器回调 |
| GET/POST | `/wechat/callback` | 迁移期旧回调兼容入口 |
| GET/POST | `/voicelife/reminder-actions/:token` | VoiceLife 产品的提醒 Action UI |
| GET | `/api/state` | 查看 Demo 全部状态 |
| POST | `/api/binding-codes` | 创建设备绑定码，可选参数二维码 |
| POST | `/api/demo/bind` | Mock 模式模拟 OpenID 绑定 |
| POST | `/api/reminders` | 创建设备提醒 |
| POST | `/api/reminders/:id/dispatch` | 手工触发发送 |

除微信回调和健康检查外，`/api/*` 使用：

```text
Authorization: Bearer <DEVICE_API_KEY>
```

## 7. Satori 接口

Satori Server 默认监听：

```text
HTTP API:  /satori/v1/{resource}.{method}
WebSocket: /satori/v1/events
```

配置：

```dotenv
SATORI_PATH=/satori
SATORI_TOKEN=一段独立随机密钥
```

调用时携带：

```text
Authorization: Bearer <SATORI_TOKEN>
Satori-Platform: wechat-official
Satori-User-ID: <WECHAT_ACCOUNT>
```

例如读取当前 Bot：

```bash
curl -X POST http://localhost:8787/satori/v1/login.get \
  -H 'Authorization: Bearer change-me' \
  -H 'Satori-Platform: wechat-official' \
  -H 'Satori-User-ID: gh_xxx' \
  -H 'Content-Type: application/json' \
  -d '{}'
```

## 8. Koishi 与微信原生能力边界

- 普通文字、图片、语音、关注和取消关注由 Koishi 适配器标准化；
- 模板消息、模板发送回执和参数二维码仍是微信专属能力；
- 微信公众号在产品上只有一个渠道、一个 WeChat Adapter；
- H5 是该渠道的 Action UI，不是 Adapter；以后替换为小程序时仍调用同一动作入口；
- H5 Route 与标准 `interaction/button` 位于 VoiceLife Koishi Plugin，
  只负责接收、校验和转发动作，不直接修改提醒状态；
- `Binding Handler` 只接收规范化的 `ExternalIdentity`，不读取 OpenID 等平台字段；
- Handler 只依赖 `IM Application`；Demo 业务实现通过 Binding Service Port 和
  Reminder Command Port 注入；
- 当前官方适配器未把 `SCAN` 和 `TEMPLATESENDJOBFINISH` 转换成标准 Session，
  Demo 在 Koishi 路由边界补充处理这两个明文事件；
- 未来平台 Adapter 将绑定相关输入规范化为 `binding.requested`，复用同一个
  `Binding Handler`；普通消息和提醒动作不会进入该 Handler；
- 以后接入其他 IM 时，通用消息进入同一个 VoiceLife Koishi 插件，平台卡片、
  模板和回执继续放在各 Adapter 能力扩展中。

## 9. Demo 边界

- JSON 文件存储仅用于 PoC，生产应换成数据库和事务 Outbox；
- Demo 为便于一条命令运行，将 Koishi Runtime 与模拟的本地 `TimingTask`
  组合在同一 Node.js 进程；两者通过端口连接，生产部署时端口可替换为 IPC/RPC，
  Handler 和 Adapter 无需修改；
- Mock 模式不代表微信真实送达；
- 模板能否用于“用户自建日程提醒”必须由真实服务号类目和模板审核确认；
- 当前语音回调只展示微信 `Recognition` 或 `MediaId`，尚未连接正式 ASR；
- 当前是 H5 操作页，不是微信小程序；快捷隧道也没有生产可用性保证；
- “知道了”是明确的业务操作，不等同于微信消息的系统已读状态；
- 微信官方适配器的 `SCAN`/模板回执补充处理当前只验证了明文模式；
- Satori 统一的是通用聊天模型，不会自动抹平各平台的模板、卡片和主动消息限制；
- MCP 接入点 URL、微信 AppSecret、AESKey 都是密钥，不要提交到 Git。

## 10. 相关资料

- [小智 ESP32 官方仓库](https://github.com/78/xiaozhi-esp32)
- [小智 WebSocket 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md)
- [小智 MCP 使用说明](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md)
- [微信公众号接收普通消息](https://developers.weixin.qq.com/doc/offiaccount/Message_Management/Receiving_standard_messages.html)
- [微信公众号接收事件](https://developers.weixin.qq.com/doc/offiaccount/Message_Management/Receiving_event_pushes.html)
- [微信公众号模板消息](https://developers.weixin.qq.com/doc/offiaccount/Message_Management/Template_Message_Interface.html)
- [Koishi 微信公众号适配器](https://koishi.chat/zh-CN/plugins/adapter/wechat-official)
- [Koishi Satori Server](https://koishi.chat/zh-CN/plugins/develop/server-satori)
- [Satori Protocol](https://satori.chat/en-US/protocol/)
