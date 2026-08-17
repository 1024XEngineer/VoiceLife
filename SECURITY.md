# 安全策略

## 支持范围

安全修复优先覆盖 `main` 和最新正式 Release。`0.x` 阶段接口仍可能变化，团队会在修复说明中写明受影响版本、临时缓解和升级方式。

## 私密报告漏洞

不要通过公开 Issue、PR 或讨论区提交未修复漏洞、凭据、设备身份和用户数据。请使用 GitHub 的 [Private Vulnerability Reporting](https://github.com/1024XEngineer/VoiceLife/security/advisories/new) 提交：

- 受影响版本或 commit；
- 攻击前提和影响；
- 最小复现或概念验证；
- 已知缓解办法；
- 是否可能已经泄露真实数据。

团队确认后会在同一私密线程协调修复、验证和披露时间。安全修复合并前不要求公开完整利用细节。

## 特别关注

VoiceLife 处理设备凭据、语音和私人日程。以下问题按安全问题报告：明文传输 bearer token、越权读取/修改日程、日志泄露、未验证 OTA、远程代码执行、持久化损坏导致跨用户数据暴露，以及 IM 动作身份或幂等绕过。
