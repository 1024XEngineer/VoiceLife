# Source 16：Linx WebSocket 与 OTA 线上契约核验

- 官方文档：<https://linx.qiniu.com/docs/xrobot/platform/websocket>
- 固定版本：`qiniu/Xrobot-docs@1b2530022e693ca1a392e6b85b9db1b12c941e49`
- 读取与只读探针日期：2026-08-04
- 类型：厂商一手文档、公开源码与合成身份线上观察

## 可以据此实现的部分

首个生产候选路径固定为 `OTA 配置 -> WSS -> hello -> JSON 控制帧/二进制音频帧`。推荐端点是 `wss://xrobo-io.qiniuapi.com/v1/ws/`；握手头至少包含 `Authorization`、`Protocol-Version`、`Device-Id`、`Client-Id`，地址允许由 OTA 动态下发。

线上只用虚构 MAC/UUID 做协议形状核验，没有使用真实设备凭据：Opus hello 返回 16 kHz、单声道、60 ms、`bits_per_sample=16`、600 ms 播放缓冲；PCM hello 返回 16 kHz、单声道、20 ms、`bits_per_sample=16`、1000 ms 播放缓冲。服务端参数不是常量，Audio Port 必须在 hello 后按协商结果打开。

## 必须保留的兼容与安全边界

- 文档示例使用 `bit_depth`，线上响应使用 `bits_per_sample`；解析器兼容别名，向核心只输出稳定的 `AudioFormat`。
- OTA 异常请求出现过 HTTP 200、业务 `code=500`；客户端必须同时检查 HTTP 和 JSON 业务码，成功配置以原子方式替换上一份可用配置。
- 未携带 `Device-Id` 时，服务端可能先完成 HTTP 101 再发送协议错误；连接成功不能替代 hello 成功。
- 合成身份在未带 Authorization 时收到 hello，只是当前部署观察，不代表生产认证可省略。token 来源、刷新、撤销和 TTL 仍未公开。
- 官方 MQTT 页面在固定版本中仍只有“待补充”。小智的 MQTT + UDP 可以研究，不能作为 Linx MQTT 已支持的证据。

## 当前项目决定

先实现 WebSocket Transport、双向音频协商、Credential Provider 和统一重连策略。MQTT/UDP 只保留扩展点；真实绑定设备的 listen、STT/TTS、打断、重连和音频闭环未完成前，README 不声明 Linx 端到端可用。
