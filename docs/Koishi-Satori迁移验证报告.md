# VoiceLife Koishi + Satori 迁移验证报告

> 验证日期：2026-07-29
> 验证对象：`demos/wechat-xiaozhi`
> 结论：可跑通，适合作为 IM Gateway；不能替代平台专属能力层。

## 1. 验证版本

| 组件 | 版本 |
|---|---:|
| Koishi | 4.18.11 |
| Koishi 微信公众号适配器 | 2.5.0 |
| Koishi Satori Server | 2.9.1 |
| Koishi Server | 3.2.9 |
| Node.js | 26.5.0 |

## 2. 实际架构

```text
微信公众号 ─→ Koishi WeChat Official Adapter ─→ Koishi Session
                                                    │
                                                    ├→ VoiceLife 业务插件
                                                    └→ Satori HTTP / WebSocket

小智 MCP ─→ VoiceLife Reminder Service ─→ 微信模板消息 API
                                         ├→ 模板发送回执
                                         └→ H5 知道了 / 推迟 10 分钟
```

通用消息由 Koishi/Satori 统一；模板消息、参数二维码、微信验签、
模板回执和 H5 动作仍位于微信能力边界。

## 3. 已完成的真实验证

| 验证项 | 结果 | 证据 |
|---|---|---|
| 微信 URL 校验 | 通过 | `/wechat-official` 正确返回 `echostr` |
| 微信文字消息进入 Koishi | 通过 | VoiceLife Koishi 中间件收到真实 OpenID 会话 |
| 被动文字回复 | 通过 | 正确签名的微信回调返回 200 和被动回复 XML |
| Satori Bot 发现 | 通过 | `login.get` 返回 `wechat-official` Bot |
| Satori 实时事件 | 通过 | WebSocket 收到 `message-created` |
| 小智 MCP | 通过 | 启动后连接 `api.xiaozhi.me` |
| 微信模板发送 | 通过 | 微信返回精确字符串 `msgid` |
| 模板发送回执 | 通过 | `deliveryStatus=success` |
| H5“知道了” | 通过 | 提醒由 `sent` 更新为 `dismissed` |
| 自动测试 | 通过 | 20 项测试全部通过 |

最终闭环：

```text
小智/HTTP 创建提醒
  → 微信模板消息
  → TEMPLATESENDJOBFINISH success
  → 用户点击 H5“知道了”
  → reminder.status = dismissed
```

## 4. 迁移收益

1. 普通文字、图片、语音和用户事件进入统一 Koishi Session。
2. Satori 提供统一 HTTP API 与 WebSocket 事件出口。
3. 后续增加飞书、钉钉、Telegram 等平台时，可复用 VoiceLife Koishi 插件。
4. VoiceLife 的提醒、设备和用户绑定模型没有依赖 Koishi Session，可独立测试。
5. 微信模板消息和 H5 快捷操作没有因迁移而丢失。

## 5. 实测发现的边界

### 5.1 微信公众号适配器不是完整平台能力封装

当前适配器负责普通消息接收和被动/客服回复，但没有统一封装：

- 模板消息发送；
- `TEMPLATESENDJOBFINISH`；
- `SCAN`；
- 参数二维码业务；
- H5 或小程序动作；
- VoiceLife 业务回执。

因此仍需要 `WechatOfficialCapability` 一类的平台扩展服务。

### 5.2 需要额外的微信验签层

对当前 2.5.0 适配器源码检查发现：

- 明文 POST 分支没有自行校验 `signature`；
- 安全模式 URL 验证不完整。

Demo 在 Koishi 路由之前复用了原有微信验签和解密逻辑。生产环境不能直接把
未补强的明文 Webhook 暴露到公网。

### 5.3 部分微信事件不会成为标准 Session

`SCAN` 和 `TEMPLATESENDJOBFINISH` 当前不会被适配器转换成标准 Session。
Demo 在路由边界识别并处理这两个明文事件。安全模式下还需要继续补充测试。

### 5.4 Satori 统一的是通用消息，不是平台政策

Satori 可以统一：

- 登录状态；
- 消息事件；
- 通用消息发送；
- 用户、频道和群组等基础模型。

它不能统一：

- 微信模板审核；
- 主动消息时间窗口；
- 各平台卡片格式；
- 小程序跳转；
- 各平台回执语义。

## 6. 运行时与依赖风险

1. Koishi 4.18 顶层 ESM Loader 在本机 Node.js 26.5.0 下出现互操作异常。
   Demo 统一通过 CommonJS 入口加载 Koishi 和官方插件。
2. `npm install` 报告 14 个 moderate 级传递依赖问题，并提示
   `@koa/router@10.1.1` 已弃用。进入生产前需要单独审计，不能直接执行
   可能带来破坏性升级的 `npm audit fix --force`。
3. Cloudflare Quick Tunnel 只适合验证，正式环境需要固定 HTTPS 域名。

## 7. 建议结论

推荐采用：

```text
VoiceLife Core
  ├→ Koishi/Satori Gateway：通用会话和多 IM 接入
  └→ Platform Capabilities：模板、卡片、回执、二维码、小程序
```

不建议让业务核心直接持有 Koishi Session，也不建议假设新增 Koishi Adapter
就能自动获得某个平台的全部主动通知能力。

本次验证说明：**Koishi + Satori 可以用于 VoiceLife，但正确定位应是可替换的
IM Gateway，而不是提醒领域模型或平台专属能力的替代品。**

## 8. 参考

- [Koishi 官方适配器列表](https://koishi.chat/zh-CN/plugins/)
- [Koishi 微信公众号适配器](https://koishi.chat/zh-CN/plugins/adapter/wechat-official)
- [Koishi 适配器模型](https://koishi.chat/zh-CN/guide/adapter/)
- [Koishi Satori Server](https://koishi.chat/zh-CN/plugins/develop/server-satori)
- [Satori Protocol](https://satori.chat/en-US/protocol/)
