# VoiceLife 微信公众号 × 小智 Demo

这是一个零第三方依赖的最小验证服务，串起：

```text
小智语音 → MCP 工具 → VoiceLife 提醒 → 微信公众号模板消息
                                          ↓
                 点击消息 → H5“知道了/推迟 10 分钟”
                                          ↓
                                  VoiceLife 状态
```

已实现：

- 微信公众平台服务器 URL 校验；
- 明文模式和 AES 安全模式；
- 接收文字、语音、关注、扫码、取消关注事件；
- 用短绑定码或带参数二维码绑定小智设备与公众号 OpenID；
- 定时提醒和模板消息发送；
- `TEMPLATESENDJOBFINISH` 发送完成事件；
- 模板消息跳转到带签名的 H5 快捷操作页；
- 网页一键“知道了”或“推迟 10 分钟”；
- 公众号回复“关闭”“推迟 10 分钟”；
- 小智 MCP 接入点 WebSocket 客户端；
- 生成绑定码、创建/查询/关闭/推迟提醒五种 MCP 工具；
- 本地 JSON 持久化、发送幂等和 Mock 模式。

## 1. 环境要求

- Node.js 22 或更高版本；
- 实体小智设备；
- 小智官方控制台或兼容 MCP 接入点的自建服务；
- 微信公众平台测试号，或者认证服务号；
- 真实微信联调时需要一个公网 HTTPS 地址。

项目没有 npm 运行时依赖，不需要执行 `npm install`。

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

公众号收到消息时，服务端应出现：

```text
[wechat] 收到消息回调：mode=plain bytes=...
[wechat] 回调处理成功：replyBytes=...
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

- AppID；
- AppSecret；
- 自己设置的 Token；
- EncodingAESKey（如果启用安全模式）；
- 模板 ID 和模板字段名。

### 4.2 暴露公网 HTTPS

微信服务器必须能访问本机的：

```text
https://你的公网域名/wechat/callback
```

可使用已有服务器反向代理，也可以在开发阶段使用可信的 HTTPS Tunnel。Tunnel 应转发到本机 `8787` 端口。

设置：

```dotenv
BASE_URL=https://你的公网域名
WECHAT_TOKEN=与公众平台服务器配置完全一致的Token
```

公众平台“服务器配置”填写：

```text
URL:      https://你的公网域名/wechat/callback
Token:    与 WECHAT_TOKEN 相同
消息加密: 初次验证可选明文；正式建议安全模式
```

安全模式还需要：

```dotenv
WECHAT_APP_ID=wx...
WECHAT_AES_KEY=公众平台的43字符EncodingAESKey
```

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
WECHAT_TEMPLATE_DETAIL_URL=https://你的公网域名/reminders/action
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
公众号文字回复“关闭”或“推迟10分钟”仍然保留，作为兼容入口。

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

公众号没有通用已读回执。用户点击 H5 的“知道了/推迟 10 分钟”或发送对应文字，才是 VoiceLife 可确认的业务回执。

## 6. HTTP API

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/health` | 配置与健康状态 |
| GET/POST | `/wechat/callback` | 微信校验、消息与事件回调 |
| GET/POST | `/reminders/action` | 带签名的提醒快捷操作页 |
| GET | `/api/state` | 查看 Demo 全部状态 |
| POST | `/api/binding-codes` | 创建设备绑定码，可选参数二维码 |
| POST | `/api/demo/bind` | Mock 模式模拟 OpenID 绑定 |
| POST | `/api/reminders` | 创建设备提醒 |
| POST | `/api/reminders/:id/dispatch` | 手工触发发送 |

除微信回调和健康检查外，`/api/*` 使用：

```text
Authorization: Bearer <DEVICE_API_KEY>
```

## 7. Demo 边界

- JSON 文件存储仅用于 PoC，生产应换成数据库和事务 Outbox；
- Mock 模式不代表微信真实送达；
- 模板能否用于“用户自建日程提醒”必须由真实服务号类目和模板审核确认；
- 当前语音回调只展示微信 `Recognition` 或 `MediaId`，尚未连接正式 ASR；
- 当前是 H5 操作页，不是微信小程序；快捷隧道也没有生产可用性保证；
- “知道了”是明确的业务操作，不等同于微信消息的系统已读状态；
- MCP 接入点 URL、微信 AppSecret、AESKey 都是密钥，不要提交到 Git。

## 8. 相关资料

- [小智 ESP32 官方仓库](https://github.com/78/xiaozhi-esp32)
- [小智 WebSocket 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md)
- [小智 MCP 使用说明](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md)
- [微信公众号接收普通消息](https://developers.weixin.qq.com/doc/offiaccount/Message_Management/Receiving_standard_messages.html)
- [微信公众号接收事件](https://developers.weixin.qq.com/doc/offiaccount/Message_Management/Receiving_event_pushes.html)
- [微信公众号模板消息](https://developers.weixin.qq.com/doc/offiaccount/Message_Management/Template_Message_Interface.html)
