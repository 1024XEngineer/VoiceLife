# PCB 小智全板载 C++ 迁移方案

## 结论与下一步

本方案把 #62 的业务能力整体迁移到 ESP32：日程、提醒、临时记录、撤销、回执、持久化和定时调度都在板上运行。灵矽云端只做 ASR、LLM、MCP 工具调用和 TTS，不再调用外部 #62 TypeScript 服务。

下一步先用官方 `bread-compact-wifi` 配置编译一份不带业务改动的基线，确认当前 16 MB Flash/8 MB PSRAM 的分区和应用尺寸；再按下面的接口加板载服务。当前已完成原始 Flash 备份，尚未写入任何新固件。

## 1. 运行时边界

```text
用户语音
  -> 小智 C++ 音频管道（I2S、唤醒、Opus）
  -> 灵矽 WebSocket
  -> 灵矽 ASR + Agent/LLM
  -> 小智设备 MCP tools/call
  -> ESP32 VoiceLife 服务 + 本地数据库
  -> MCP 结果（speech、结构化字段、回执 ID）
  -> 灵矽 TTS
  -> 小智 C++ 功放播放
```

提醒主动播报走同一条云端语音链路：本地定时器到期后，设备向当前灵矽会话注入一条带有 `【系统到期播报】` 标记的文本；Agent 只复述文本，TTS 音频回到板上。断网时保存提醒状态和回执，联网后再处理，不在板上另实现 ASR 或 TTS。

## 2. 产品范围

目标行为与 #62 保持一致：

- 单次、每天、每周、每月日程的创建、查询和冲突确认；
- 日程查找、仅本次/本次及以后/整个系列修改；
- 跳过、暂停、恢复、终止和删除；
- 主提醒、提前 15 分钟弱提醒、关闭、推迟和详情；
- 24 小时非敏感临时记录；
- 最近 10 分钟的撤销；
- 操作回执、错误回执和设备离线后的待处理状态。

首个硬件演示先走三条验收路径：创建日程、查询日程、到期提醒并关闭/推迟。其余工具必须从第一版接口设计中保留，按独立 PR 逐步填实，不能再改成外部服务调用。

明确不做：多用户、第三方日历同步、完整 RRULE、农历/工作日、跨时区、板上完整 Web 管理界面、云端数据库。回执先存板上并通过语音返回；飞书/微信推送另列后续适配器。

## 3. C++ 模块边界

建议在官方工程中新增一个独立组件（暂定 `components/voicelife`），不修改官方 `bread-compact-wifi` GPIO 定义：

| 模块 | 职责 | 对外接口 |
|---|---|---|
| `domain` | Event、Occurrence、Reminder、Note、Receipt、UndoOperation 和周期/时间区间规则 | 纯 C++ 类型与无副作用规则函数 |
| `storage` | SQLite 数据库、迁移、事务、断电恢复 | `Storage::transaction()`、各实体 Repository |
| `calendar` | 创建、查询、查找、冲突检测和周期实例变更 | `CalendarService` |
| `reminder` | 主/弱提醒状态、关闭、推迟次数和重启恢复 | `ReminderService` |
| `notes` | 24 小时临时记录和敏感词拒绝 | `ShortNoteService` |
| `undo` | 10 分钟快照和恢复 | `UndoService` |
| `receipt` | 结果、错误、播报文本和历史记录 | `ReceiptStore` |
| `scheduler` | SNTP 对时、到期扫描、队列和主动播报 | `ReminderScheduler`、`CloudVoicePort` |
| `mcp` | 将领域服务暴露为灵矽可调用的设备工具 | `VoiceLifeMcpAdapter` |

所有领域写操作经过同一个服务任务和事务锁，避免 MCP 回调、提醒扫描和撤销同时修改同一条记录。

## 4. 数据模型与存储

使用 SQLite 保持 #62 的事务语义；数据库文件放在单独的 `voicelife` 数据分区，不放 NVS。时间统一存 UTC epoch milliseconds，展示时使用 `Asia/Shanghai`。

最小表集合：

- `events`：标题、类型、开始/结束、地点、备注、周期规则、状态；
- `event_exceptions`：跳过、单次修改和周期分割；
- `reminders`：触发时间、主/弱类型、状态、推迟次数；
- `short_notes`：内容、分类、过期时间；
- `undo_operations`：操作类型、快照、确认令牌、过期时间；
- `receipts`：工具结果、播报文本、结构化 JSON 和发送状态；
- `meta`：schema 版本、最近一次 SNTP 同步时间和设备实例 ID。

启动时执行迁移；提醒只物化近期实例，不把无限周期展开成大量行。SQLite 使用单写者、显式事务和断电后回滚策略，禁止在音频任务里直接做长查询。

## 5. 设备 MCP 合同

保留 #62 工具名和关键返回字段，让灵矽 Agent 提示词可以复用：

`calendar_create`、`calendar_query`、`calendar_find`、`calendar_modify`、`calendar_reschedule_occurrence`、`calendar_skip_occurrence`、`calendar_pause_series`、`calendar_resume_series`、`calendar_terminate_series`、`calendar_delete`、`calendar_undo`、`reminder_list_due`、`reminder_close`、`reminder_snooze`、`reminder_get_details`、`note_record`、`note_query`。

返回字段保持 `ok`、`speech`、`requiresConfirmation`、`confirmationToken`、`undoOperationId`、`receiptId` 等语义。当前官方 `PropertyList` 只覆盖字符串/整数/布尔值，因此需要给 `McpTool` 增加原始 cJSON schema/参数入口，支持周期对象、冲突列表和确认令牌，不把复杂参数偷偷压成不可验证的字符串。

## 6. Flash 与分区决策

开发时先照搬官方 `bread-compact-wifi` 的硬件配置和显示配置。分区不在未测尺寸前硬改：

1. 先用官方当前工程完成 stock build，记录应用和 assets 实际大小；
2. 在 16 MB 布局保留双 OTA，优先从 assets 空间中划出 1.5–2 MB `voicelife` 数据分区；
3. 如果应用加入 SQLite 后超过 4 MB，再调整为 5 MB OTA 槽并重新计算 assets，不牺牲恢复镜像和 OTA 回滚；
4. 每次改变分区表都先备份，再做空数据库启动和断电恢复测试。

当前设备原始分区中已经能看到约 2 MB 的尾部空闲区，但正式开发仍以新固件生成的 partition table 为准。

## 7. 开发顺序与门槛

### A. 基线

- 使用官方仓库当前主线和其要求的 ESP-IDF 版本编译 `bread-compact-wifi`；
- 不加 VoiceLife 代码先完成启动、I2S 录放、OLED、LED、旋钮和 WebSocket；
- 保留 `v0.9.7_bread-compact-wifi` 和本次整片备份作为回退基线。

### B. 架构骨架

- 先提交 `domain/storage/service/mcp/scheduler` 的可编译接口和空实现；
- 用一条 `calendar_create -> SQLite -> MCP result -> TTS` 主路径串起来；
- 骨架通过 Issue/PR Review 后再填规则，不把生成代码直接当架构结论。

### C. MVP 填实

- 创建/查询/冲突确认；
- 提醒扫描、关闭、推迟；
- 回执落库和主动播报；
- 再补周期变更、临时记录和撤销。

### D. 质量门槛

- 模拟时钟覆盖自然日边界、重叠冲突、周期分割、第三次推迟和撤销过期；
- 设备重启后数据库、提醒状态和 schema 迁移可恢复；
- Wi-Fi 断开时不丢写入，恢复后不会重复播报；
- 端到端日志能对应每个 MCP request、receipt 和 TTS turn。

## 8. 主要风险

- SQLite 组件与当前 ESP-IDF 版本的兼容性和固件体积；
- 官方 MCP schema 当前只支持标量属性，需要扩展后才能准确表达 #62 的复杂参数；
- 主动播报依赖灵矽会话注入，需验证设备侧 `listen/detect` 注入和打断状态机；
- 无硬件 RTC 时，断电后必须等 SNTP 对时，不能凭旧时间误触发；
- 16 MB Flash 的 OTA、模型/assets 和业务数据库需要重新做容量预算。

这些风险在编码前分别做小实验，不把不确定性留到整机联调阶段。
