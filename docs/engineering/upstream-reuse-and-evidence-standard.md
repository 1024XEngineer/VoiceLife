# 上游复用与证据驱动开发规范

结论：VoiceLife 的设备能力优先复用已经被官方或真实项目验证过的实现，不以“自己写一遍”作为工程能力证明。复用必须经过来源、许可证、接口边界和目标板证据四道检查；无法证明的热门代码只能作为线索，不能直接进入固件。

下一步动作：每个涉及 ESP、音频、模型、网络协议或第三方库的 Issue/PR，都按本文先填写复用决策表，再开始编码。代码、主机测试、目标构建和实板证据仍需分层验收；真实人声测试是语音链路的最后一项，不得被静音日志或云端关键词命中替代。

## 1. 适用范围

本文适用于：

- ESP-IDF 组件、ESP-SR/AFE/MultiNet、音频队列、I2S、WebSocket、OTA、MCP 和模型打包；
- 从小智、MVP、官方示例、成熟开源项目或供应商 SDK 迁移代码、配置、模型和测试方法；
- 因时限需要采用现成方案，而不是重新设计等价功能的情况。

本文不改变产品边界、组件依赖方向或 GitHub 的 Proposal -> Design -> Coding -> Review -> Merge 流程。它解决的是“实现从哪里来、怎么判断可以拿来用”。

## 2. 复用决策顺序

候选实现按以下顺序查找，找到更高等级的证据后，不再优先采用低等级的自研替代：

| 优先级 | 来源 | 可以直接得到什么 | 必须补什么 |
| --- | --- | --- | --- |
| 1 | 芯片/协议供应商官方 SDK 和文档 | API、模型格式、硬件限制、升级兼容性 | 在当前 IDF、Profile 和板卡上构建/验证 |
| 2 | 官方示例或长期维护的参考实现 | 任务调度、缓冲、错误处理、测试方法 | 去除应用耦合，接入 VoiceLife Port |
| 3 | 当前仓库已有 MVP 或已验收模块 | 板卡引脚、实际分区、串口操作和回退流程 | 重新核对当前架构契约，不能把日志当验收 |
| 4 | 活跃开源项目的稳定版本 | 可运行的实现和边界案例 | 固定 commit、查许可证、做差异审查和目标板测试 |
| 5 | 博客、论坛、短代码片段 | 问题线索和候选方法 | 至少找到一份一手文档或可重复实验后才允许采用 |

“网上很火”本身不是证据。收藏量、点赞数和一段成功日志不能证明模型、任务优先级或分区布局适合 VoiceLife。

## 3. 四种处理方式

每个候选实现必须明确选一种方式，并写出理由：

### A. 直接复用

满足以下条件才允许：接口和所有权语义一致，许可证兼容，目标芯片/IDF 版本覆盖，且可以用当前构建或测试复现。只保留必要的配置和文件，不把上游无关的板卡矩阵、UI、业务状态带入。

### B. 适配复用（默认选择）

上游能力正确，但接口带有自己的类型或全局状态时，用 VoiceLife Port/Adapter 包起来。适配层负责字段、线程和生命周期转换；领域模块不能看到 ESP-IDF、XRobot 或上游类型。

例如：小智的 `CustomWakeWord`/AFE 负责 MultiNet 输入与模型生命周期，VoiceLife 只暴露 `LocalWakeDetectorPort`；小智的 `Protocol::SendWakeWordDetected` 对应 Linx Provider 的 `NotifyLocalWakeWord`，不复制其 `Application` 状态机。

### C. 参考重写

只有在许可证、硬件、协议或资源模型不兼容时才重写。重写前必须保留上游行为清单和对照 fixture；能复用的算法、模型格式、边界条件和测试数据继续复用，不得把“不喜欢代码风格”当作重写理由。

### D. 拒绝采用

以下任一情况直接拒绝：来源无法定位、许可证不清楚、包含凭据/隐私路径、依赖未维护且无替代、只证明了模拟器、或迁移后会绕过安全/回滚边界。拒绝理由写入 Design Issue，避免下一个人重复调查。

## 4. 复用决策表

开始编码前，在 Issue 或 PR 描述中填写：

| 字段 | 最低要求 |
| --- | --- |
| 来源 | URL、仓库、文件/章节、版本或完整 commit |
| 许可证 | SPDX 标识或许可证文件路径；不确定时停止迁移 |
| 原行为 | 输入、输出、线程/回调、内存所有权、错误和超时 |
| VoiceLife 边界 | 落在哪个组件、哪个 Port/Adapter；不把上游类型带进核心 |
| 迁移内容 | 复用文件/函数/配置/model；明确未迁移的部分 |
| 证据 | 上游测试/文档、当前主机测试、目标构建、实板结果分别列出 |
| 成本 | 直接复用、适配、重写三种方案的粗略文件数/工作量；说明选型 |
| 回退 | 依赖、分区、固件和配置如何恢复；是否需要擦写或不可逆硬件操作 |
| 敏感信息 | 证明日志、fixture 和备份不包含 token、Wi-Fi、原始语音、设备身份或 MCP 参数 |

适配层预计不超过约 200 行且不改变领域语义时，优先适配；如果适配明显超过重写成本，先做空骨架和对照测试，再让 Reviewer 决定。这个数字是 Review 触发器，不是绕过设计的硬阈值。

## 5. 证据分层与通过线

“编译通过”只证明一层事实，不能覆盖整条设备链。按照以下顺序收集证据：

1. **来源证据**：版本、commit、许可证和官方限制可复查。
2. **契约证据**：主机测试证明接口、顺序、失败和代次语义；待机帧不得进入云端 Provider。
3. **构建证据**：目标 IDF/芯片/Profile 构建通过，依赖 lock 未漂移，镜像和模型小于分区。
4. **设备证据**：非人声验证模型加载、任务启动/停止、WSS hello、资源水位、回退和分区回读。
5. **真实场景证据**：真实网络、真实服务、真实人声或真实外设行为。只在前四层通过后执行，且记录脱敏摘要。

通过线按行为范围匹配证据范围：

- 宣称“API 接入”至少需要来源 + 契约 + 构建；
- 宣称“板上可运行”必须再有设备证据；
- 宣称“语音闭环”必须有真实 STT、MCP、TTS 和恢复证据；
- 宣称“你好牛牛唤醒可用”必须证明本地声学检测、`listen.detect -> listen.start` 顺序、待机隔离和 TTS 后再次唤醒。

主机 mock、静音 PCM、历史日志和云端 STT 关键词匹配都不能替代最后一层。

## 6. 音频/模型复用的具体规则

当前 VoiceLife 的 ESP32-S3 语音实现按以下已核对事实执行：

- ESP-SR MultiNet6/7 输入为 16 kHz、16-bit、mono；命令 ID 从 1 开始；API 增删命令后必须调用 `esp_mn_commands_update()`。
- Espressif 建议 MultiNet 与 AFE 一起使用。若当前 PCM 端口暂时直接喂 MultiNet，必须在文档和实板验收中标为风险，不能宣称等价于 AFE 输出；下一步优先迁移经过板卡验证的 AFE 配置。
- 模型通过独立 `model` 分区加载；分区偏移按 4 KB 对齐，应用分区按 64 KB 对齐。变更分区表属于受控部署动作，先备份、构建、回读和恢复，不在普通功能测试中盲刷。
- `dependencies.lock` 由 Component Manager 生成；Manifest 固定直接依赖版本，不能把 managed component 大目录手工复制进仓库代替依赖声明。
- 唤醒检测回调只产生本地事件。网络发送、Linx 控制消息和会话状态迁移由控制任务完成，不能阻塞 I2S/AFE 采集任务。

### 当前仓库的推荐复用映射

| 能力 | 采用来源 | VoiceLife 处理 |
| --- | --- | --- |
| MultiNet 命令注册与分块 | ESP-SR 官方 API、小智 `CustomWakeWord`、MVP `custom_wake_word.cc` | 适配为 `EspMultiNetWakeDetector`，只注册“你好牛牛” |
| AFE/唤醒任务生命周期 | 小智 `afe_audio_engine.*` 与 ESP-SR AFE 文档 | 迁移算法和配置，不迁移 `Application`/UI/板卡总状态 |
| XRobot listen detect | 小智 `Protocol::SendWakeWordDetected` 与 Linx 文档 | Provider 语义化编码，控制任务保证顺序 |
| 模型打包 | ESP-SR `movemodel.py`/`pack_model.py` | 由 Component Manager/CMake 生成 `srmodels.bin`，不复制 272 MB managed tree |
| I2S/队列/回退 | 当前 `voicelife_audio_esp` + 实板 MVP 证据 | 只保留实际 Profile 所需端口和恢复脚本 |

## 7. 最小执行流程

每个外部能力按这个短流程走，避免研究和编码互相等待：

1. 写一个真实场景和验收条件，列出本期不做项。
2. 搜索官方实现、成熟参考实现和仓库已有代码；确定来源版本和许可证。
3. 建立输入/输出对照表，先定义 VoiceLife Port 或 Adapter。
4. 写最小失败测试（RED），固定上游行为 fixture；不要为了等硬件而先写大段移植代码。
5. 适配或直接复用，保持一次变更一个结果；每个提交可编译、可回退。
6. 运行主机门禁、目标构建和非人声设备测试；失败时记录事实，不扩大到无关故障假设。
7. 最后做真实场景测试，生成脱敏证据；把未覆盖的风险写进 PR 和候选 Issue/PR 清单。

## 8. Review 检查清单

Reviewer 只需逐项回答“是/否/不适用”：

- 来源版本、许可证和迁移文件是否可定位？
- 复用内容是否真的比等价自研更省时间或风险？
- 是否只迁移当前 Profile 需要的能力？
- 上游全局状态、平台类型、凭据和业务规则是否被隔离？
- Port 的所有权、并发、背压、错误和关闭顺序是否清楚？
- 是否有主机失败测试、目标构建和适用的实板证据？
- 模型、分区、依赖锁和回退是否可重复？
- 日志和证据是否脱敏？
- PR 是否保持小而自洽，并明确未覆盖范围？

任何一项为“否”，PR 只能标为 Draft 或返回 Design Issue；不能用 CI 绿色掩盖证据缺口。

## 9. 依据与核对记录

以下来源在 2026-08-10 复核，链接指向公开的一手文档或固定版本：

- Espressif ESP-SR MultiNet command recognition：<https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html>（输入格式、AFE 建议、命令 API、分块限制）
- ESP-SR 2.4.7 manifest/commit：<https://github.com/espressif/esp-sr/tree/2f8c4b0459db5bbb39abd77adae27962d6d94bcb>（版本、模型和许可证）
- ESP-IDF partition tables：<https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/partition-tables.html>（自定义分区、对齐、大小检查和刷写）
- IDF Component Manager manifest：<https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/manifest_file.html>（版本、Git commit、依赖声明）
- FreeRTOS Kernel：<https://github.com/FreeRTOS/FreeRTOS-Kernel>（官方内核和固定版本消费方式）
- Google Engineering Practices, Small CLs：<https://google.github.io/eng-practices/review/developer/small-cls.html>（小而自洽的 Review 变更）
- 小智 ESP32 参考实现：<https://github.com/78/xiaozhi-esp32/tree/dd99da00dc4c89ed4ab07fcec038c03f13f4de50>（音频/协议/构建迁移基线，MIT）

本地对照来源：`资料查找3.0/xiaozhi-esp32-latest`、`voicelife-pcb-native-mvp` 和当前 VoiceLife Profile。它们是工程证据，不是正式构建依赖。
