# VoiceLife PCB MVP 验证矩阵

一句话结论：板端日程、提醒、周期和持久化现在用主机确定性测试覆盖，主动播报再用真实 PCB + 灵矽 WebSocket 证据确认；IM 不在本轮验收门槛内。

下一步动作：每次改动先运行 `firmware/scripts/run_voicelife_host_tests.sh`，再运行 `firmware/scripts/voicelife_hardware_smoke.py`；只有串口、回读和恢复都通过，才把真机 MVP 标为通过。

## 验收门槛

| ID | 场景 | 通过标准 | 验证层 | 当前状态 |
| --- | --- | --- | --- | --- |
| MVP-01 | 创建单次日程 | 返回 `ok=true`、事件 ID、主提醒和回执 | C++ host | PASS |
| MVP-02 | 查询日程 | 返回正确标题、时间、地点，范围右端不包含 | C++ host | PASS |
| MVP-03 | 到期主动播报 | 设备日志出现 `Delivering`、LinX 握手、TTS 音频包和 `pushed` | PCB + Linx | PASS（20260729 postflash smoke） |
| MVP-04 | 播报后重启 | 状态保留为 `pushed`，重启不重复播报 | PCB + Flash 回读 | PASS（20260729 postflash smoke） |
| MVP-05 | 关闭/推迟 | 关闭不改日程；推迟 1 到 1440 分钟，最多三次 | C++ host | PASS |
| MVP-06 | 网络/音频失败重试 | 回调失败时保留待处理状态，恢复后只成功播报一次 | C++ host | PASS |
| MVP-07 | 周期日程 | daily/weekly/monthly 查询展开，月末跳过不存在日期 | C++ host | PASS |
| MVP-08 | 周期下一次提醒 | 当前实例送达后生成下一次主/弱提醒 | C++ host | PASS |
| MVP-09 | 单次跳过 | 只跳过指定周期实例，下一次仍存在；十分钟内可撤销 | C++ host | PASS |
| MVP-10 | Flash 回退 | 真机测试前后数据分区字节可回读，恢复后设备正常启动 | esptool + serial | PASS（postflash 字节一致） |
| MVP-11 | PCM 语音创建 | ASR、`calendar_create`、日期参数和 TTS 音频完整通过 | Linx PCM + MCP fixture | PASS |
| MVP-12 | PCM 多场景 | 查询、修改、冲突、记录和安全边界功能通过 | Linx PCM + MCP fixture | PARTIAL（7/8） |
| MVP-13 | 缺时长会议 | 不猜测时长，只追问结束时间或时长 | Linx PCM + MCP fixture | FAIL（模型猜 60 分钟） |
| MVP-14 | 回复整洁度 | 不播报推导、Prompt 原句或工具选择理由 | Linx PCM + TTS | FAIL（严格 4/8） |

## 回归矩阵

| 类别 | 覆盖项 |
| --- | --- |
| 时间 | ISO-8601 `Z`/偏移/小数秒、闰年、非法日期、边界秒、过去时间 |
| 创建 | 空标题、非法时间、提醒晚于事件、提醒已过、结束时间/时长互斥、point/time_block |
| 冲突 | 点对点、点落入时间段、时间段重叠、相邻不冲突、错误确认令牌 |
| 周期 | 每日、每周星期校验、每月日期校验、31 号跳过短月、暂停/恢复/终止 |
| 变更 | 标题/时间修改、时间段结束随开始时间移动、提前量保留、跳过一次、撤销 |
| 提醒 | 主提醒、提前 15 分钟、关闭幂等、推迟边界、三次上限、批量播报 |
| 存储 | 重启恢复、提交失败回滚、状态快照、旧状态字段缺省兼容 |
| 离线 | 音频回调失败、再次 Tick 重试、成功后不重复播报 |
| 临时记录 | 正常查询、敏感词拒绝、24 小时过期 |

## 真机脚本证据

`voicelife_hardware_smoke.py` 的行为固定为：

1. 校验整片原始备份 SHA-256；
2. 只读回 `0xE00000..0xFFFFFF` 的 2 MiB `voicelife` 分区；
3. 生成 20 秒以上未来到期的 `state.json` SPIFFS 镜像并写入同一分区；
4. 监听启动、Linx、TTS、音频包和 `pushed` 日志；
5. 回读分区并检查持久化状态；
6. 默认写回第 2 步的原始分区，绝不执行 `erase_flash`。

本次新固件的真机证据保存在 `test-evidence/20260729-mvp-smoke-postflash/`：

- `manifest.json`：脚本结果为 `pass`，恢复无错误；
- `serial.log`：包含 `VoiceLifeStorage: Loaded`、Linx WebSocket 握手、TTS 开始/音频包/停止和 `pushed`；
- `voicelife-after-restore.bin`：与刷写前快照逐字节相同。

刷写前后 NVS 的 Wi-Fi、SSID、密码、WebSocket URL 和设备 UUID 均保持；服务端刷新了 WebSocket token，这是正常的激活/会话状态更新，不是配网丢失。

真机 hardware smoke 不模拟 ASR；另有 PCM 文件流真实经过灵矽 ASR、Agent、MCP 和 TTS，但其 MCP 返回为确定性夹具，不写 PCB。PCM 测试仍不能替代板载麦克风、唤醒词和实际听感验证。

## 不在本轮门槛内

- 微信/公众号 Live 凭据、公网 HTTPS 和 PR #85 sidecar；
- 完整 `this_and_future`/`entire_series` 分割编辑；
- 第三方日历同步、多用户和 Web 管理界面；
- 实际声压/听感（需要现场人员确认）。

当前仍未通过的观察项是物理麦克风连续链路：PCM 文件流已证明灵矽语音入口可识别并调用工具，但尚未留下“板载麦克风唤醒 -> 真机创建 -> 同一事项到期播报”的连续串口证据。完整结果和演示边界见 `mvp-comprehensive-test-plan-20260730.md`。
