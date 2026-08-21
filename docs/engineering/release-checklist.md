# Release Checklist

Host E2E 和 HIL 结果只能证明软件旅程的有限边界，不能替代真实平台和体验验收。发布 PR 必须记录 firmware commit、Gateway commit、Profile、测试账号类型和脱敏 evidence 位置。

## 自动门禁

- [ ] PR 的 Host unit/integration/Host E2E 通过，失败 evidence 已上传并完成敏感字段扫描。
- [ ] 最近一次手工 HIL 对 SparkBot、PCB 分别有结果；HIL 仍为非 required 时记录原因。
- [ ] workflow、设备标签、artifact retention 和 `retries=0` 配置未漂移。
- [ ] 失败按 product / infrastructure / device / external / configuration 分类，没有用重试掩盖。

## 真实平台验收（#132）

- [ ] 真实微信公众号绑定、强提醒通知和动作回执通过。
- [ ] H5 推迟/确认链接在真实 HTTPS origin 下通过，过期、重复点击和错误 scope 有记录。
- [ ] SSE 断线重连、Gateway 重启和 PostgreSQL 恢复通过。
- [ ] 真实 Linx/ASR/TTS 凭据在受控环境使用；凭据未进入 PR、artifact 或设备镜像。
- [ ] SparkBot voice HIL 通过：DashScope TTS -> PCM 注入 -> ASR -> Linx/WSS -> 下行 TTS/I2S/显示；记录 model、voice、Profile、固件 commit 和脱敏 evidence。
- [ ] 声学：唤醒、AEC、全双工抢话和误唤醒在真实扬声器/麦克风下观察并记录。
- [ ] 显示：SparkBot 状态、表情、字幕和物理输入在实板观察通过。
- [ ] 掉电、重启、配网和旧凭据撤销/恢复通过。

## 证据与回退

- [ ] PR 只附最小连续非敏感摘录；原始串口和音频保留在受控私有目录。
- [ ] 记录测试时间窗口、板型、固件/Gateway commit、Profile、账号类型和 evidence URL。
- [ ] 已知 flaky 或外部故障有分类和后续负责人，不标记为产品通过。
- [ ] 发布后发现回归时可暂停 HIL 手工 workflow 或将 Host E2E 降为非 required，但不得删除 journey、失败 evidence 或本清单。

关联：[#132](https://github.com/1024XEngineer/VoiceLife/issues/132)、[#288](https://github.com/1024XEngineer/VoiceLife/issues/288)。
