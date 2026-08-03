# Source 01：Linx XRobot WebSocket 协议

- URL：<https://raw.githubusercontent.com/qiniu/Xrobot-docs/main/docs/xrobot/platform/websocket.md>
- 读取日期：2026-08-04
- 类型：七牛官方文档仓库

## 原文摘录

> WSS (推荐) `wss://xrobo-io.qiniuapi.com/v1/ws/`

> 必需的请求头参数：`Authorization: Bearer <token>`、`Protocol-Version: 1`、`Device-Id`、`Client-Id`

> 建立 WebSocket 连接 -> 发送 hello -> 等待 hello 响应 -> 开始会话

> `play_buffer_duration`：设备音频播放缓冲区的时长，默认值为 1000 毫秒。

> 当服务器发送音频二进制帧时，设备端解码并播放。

> `tts stop` 的 `is_aborted` 为 `true` 时，建议设备立即清理音频缓冲区。

## 对本项目的约束

认证头、WebSocket 句柄和 JSON 字段留在 Linx/ESP Adapter。Provider hello 成功前不能打开 I2S 或发送音频；`is_aborted=true` 必须映射到输出 Flush 和 generation 失效。

## 限制

协议文档不能证明当前实板已经完成 WSS、ASR、TTS 或声学闭环；这些必须有真实板日志和资源测量。
