# 设备侧定时能力与第三方库评估

## 1. 结论

VoiceLife 的定时任务模块**暂不引入 CronJob、cron 表达式解析器或通用任务调度库**。设备侧应采用下列已具备的能力：

```text
内存 task 注册表
        -> 计算最近 trigger_at
        -> 一个 esp_timer 一次性定时器
        -> 仅通知 FreeRTOS Runner
        -> Runner 消费注册/取消命令
        -> Runner 按稳定顺序调用全部到期 task 的 callback
        -> 重新设置最近唤醒时间
```

这不是“用原生 API 代替库”的偏好，而是模块契约决定的：日程模块已经把周期规则、例外和提醒偏移计算为具体 `trigger_at`；定时任务模块只管理一次性 `task_id`，并在到点时调用 callback。因此 Cron parser 没有需要解析的表达式，CronJob 也不能替代内存注册表、单一 Runner 和稳定的批量执行语义。[定时任务模块设计](../../modules/timer-task-module.md)

当前 MVP 假设设备持续正常运行，不考虑断电、重启和断网；task 注册表及 callback 均为运行时内存状态。若后续需要跨重启恢复，应由日程模块从其持久化日程重新计算并注册 task，而不是为定时模块引入 Cron 或第二套日程存储。

评估日期：2026-08-12。库的活跃状态以各项目的公开仓库为准；引入任何候选前仍须固定 commit、复核许可证，并进行 ESP-IDF 6.0.2 构建与定时行为测试。

## 2. 可直接采用的 ESP-IDF 能力

| 能力 | 建议用途 | 采用边界 | 一手来源 |
| --- | --- | --- | --- |
| `esp_timer` 一次性定时器 | 对最近一个 `trigger_at` 设置 `esp_timer_start_once()` | 一个 Runtime Adapter 持有 timer。定时模块经 `Clock` 将绝对时间换算为相对延时；timer 只负责唤醒，不拥有 task 状态。 | [ESP Timer：一次性与周期定时器](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/esp_timer.html#one-shot-and-periodic-timers) |
| ESP Timer 的 task-dispatch callback | 发送“Runner 需要检查”的信号 | 默认回调在单一 timer task 中串行执行；必须短小、非阻塞。不得在其中调用日程 callback、执行 HTTP、IM 或语音操作。 | [ESP Timer：回调派发](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/esp_timer.html#callback-dispatch-methods) |
| FreeRTOS 直接任务通知 | callback 唤醒单一 Runner | 只传递“有工作可做”的合并信号；到期 task 的识别始终基于注册表与当前时间。 | [ESP-IDF FreeRTOS：Task Notifications](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/freertos_idf.html) |
| FreeRTOS queue | 向单写者 Runner 传递注册和取消命令 | 外部调用异步入队并返回 `accepted`；Runner 独占修改注册表，避免并发写入。队列容量和满载策略必须由 Runtime Adapter 明确规定。 | [ESP-IDF FreeRTOS：Queue API](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/freertos_idf.html) |
| ESP-IDF SNTP/系统时间 | 提供 `trigger_at` 所需的墙上时钟 | `esp_timer_get_time()` 不是日历时刻。`Clock` 应使用系统墙上时间；SNTP 校时后通知 Runner 重新计算最近唤醒。 | [ESP-IDF 系统时间](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/system_time.html) [ESP-IDF SNTP](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/system_time.html#sntp-time-synchronization) |

### 实现约束

1. timer 通知、日程模块提交注册/取消命令、SNTP 时间校正后，都唤醒同一个 Runner；Runner 先消费命令，再调用 `RunDueTasks(now)`。
2. Runner 组装全部 `trigger_at <= now` 的 task，按 `(trigger_at, task_id)` 稳定顺序逐个调用 callback；每个 task 执行前从待执行集合移除或标记为 `executing`。
3. light sleep 恢复时 ESP Timer 可能处理逾期 callback；因此 callback 只合并通知，Runner 始终按注册表与当前 `now` 判定到期，不能把“回调次数”等同于“应执行次数”。[ESP Timer：light sleep 回调](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/esp_timer.html#handling-callbacks-in-light-sleep-mode)
4. 这条链路需要真实设备验证：计时精度不保证 callback 的绝对实时性，且回调会被更高优先级任务延后。[ESP Timer：回调派发](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32/api-reference/system/esp_timer.html#callback-dispatch-methods)

## 3. 第三方候选

| 候选 | 状态与许可 | 可借鉴/可用范围 | 不适配点和结论 | 一手来源 |
| --- | --- | --- | --- | --- |
| `mariusbancila/croncpp` | 未归档；MIT；header-only C++11 cron 解析和“下一次 occurrence”计算 | 只有未来产品明确要接受 **Cron 文本** 作为外部配置格式时，才可在日程模块 Adapter 层评估其 parser 测试、非法表达式处理和 next-occurrence API。 | 当前日程模块已提供具体 `trigger_at`，定时任务模块既不接收 Cron 文本，也不拥有周期解析职责。该库按 C runtime local time 计算、没有自己的时区数据库。**当前不引入。** | [项目 README](https://github.com/mariusbancila/croncpp#readme) [许可证](https://github.com/mariusbancila/croncpp/blob/master/LICENSE) [仓库元数据](https://api.github.com/repos/mariusbancila/croncpp) |
| `libical/libical` | 未归档；MPL-2.0 或 LGPL-2.1 双许可；实现 iCalendar 数据与协议 | 若日程模块将来必须在设备侧完整解析/序列化 RFC 5545 iCalendar，再单独做容量、CMake 和许可证评估。 | 面向完整 iCalendar 协议，依赖和裁剪面显著大于本模块需求；不会提供 ESP 定时唤醒或 task 批量执行。定时任务模块不保存 RRULE/时区副本，因此**不作为其依赖。** | [项目 README 与许可说明](https://github.com/libical/libical#readme) [仓库元数据](https://api.github.com/repos/libical/libical) |
| `exander77/supertinycron` | 未归档；Apache-2.0；提供 Cron expression parser，并带面向命令行的 tinycron 工具 | 可作为“Cron DSL 仍有解析实现复杂度”的反例和未来 parser 选型的比较对象。 | README 明确包含独立 `supertinycron` 命令行工具，且其日历解释以 C runtime / Cron 语义为中心。当前模块的输入是具体时间点，并需要单一 Runner、稳定批次顺序和取消语义；它不匹配这些职责。**不引入。** | [项目 README](https://github.com/exander77/supertinycron#readme) [许可证与仓库元数据](https://api.github.com/repos/exander77/supertinycron) |
| `DavidMora/esp_cron` | 未归档；Apache-2.0；最后推送时间应以仓库 API 为准 | 可借鉴它把调度工作放进 FreeRTOS task 的基本形态。 | README 要求调用方先初始化 `time.h`；源码在内存 job 表上计算 Cron 的下一次时间，并为 callback 创建任务。当前模块不解析 Cron，且需要由单一 Runner 串行消费命令和按稳定顺序执行到期 task；额外 task 创建会破坏该单写者模型。**明确不采用。** | [项目 README](https://github.com/DavidMora/esp_cron#readme) [调度源码](https://github.com/DavidMora/esp_cron/blob/master/cron.c) [许可证与仓库元数据](https://api.github.com/repos/DavidMora/esp_cron) |

## 4. 明确排除 CronJob

“CronJob”在这里可能指 Kubernetes CronJob、Unix cron daemon，或嵌入式 cron callback 库；三者都不应进入设备提醒主链路：

- Kubernetes CronJob 是集群控制面的批任务资源，不在 ESP-IDF 设备运行时中执行，不能用于板上离线提醒。[Kubernetes CronJob 文档](https://kubernetes.io/docs/concepts/workloads/controllers/cron-jobs/)
- Cron 表达式描述重复的墙上时间匹配；当前定时模块只接受日程模块计算好的具体 `trigger_at`，因此没有 Cron 表达式的输入或解析职责。
- 即使 Cron 库支持秒级表达式，它的 callback 仍不能替代单一 `esp_timer`、Runner 命令队列，以及全部到期 task 的稳定批量执行。
- 用“每分钟 Cron 扫描”做主路径会增加无效唤醒，并把已有的最近到期点精确计算退化为轮询。只有未来新增**云端**运维进程时，才可将 Cron 作为触发可重入扫描的外层机制；该机制不拥有领域状态，也不改变设备侧架构。

## 5. 采用清单

本次可直接落实为一个可独立验证的 Runtime 切片：

1. 新建 ESP-IDF Runtime Adapter，封装一个 task-dispatch 的一次性 `esp_timer`。
2. timer callback 只通过任务通知唤醒 Runner；外部 `RegisterTask` / `CancelTask` 命令也由 Queue 唤醒 Runner。Runner 消费命令、调用 `RunDueTasks(now)`，再按 `next_wake_at` 重新 arm timer。
3. 通过主机测试覆盖同一时刻 task 的稳定顺序、callback 内取消尚未执行 task、命令队列单写者语义，以及 SNTP 校时后的最近唤醒重算。
4. 验证 timer 重设失败和命令队列满载的可观察错误与调用方处理方式；不要新增 Cron 依赖。

该切片不会改变日程的 RRULE/时区所有权，也不会把日程 callback、语音、IM 或 HTTP 调用放入 timer callback。
