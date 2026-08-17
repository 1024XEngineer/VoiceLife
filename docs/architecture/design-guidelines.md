# VoiceLife 架构与适配器设计规范

本规范把 VoiceLife 固定为“纯 C++ 业务核心 + ESP-IDF 组件化模块单体 + Ports/Adapters”。新增平台、板卡或基础设施时，应增加适配器和配置，不得把外部类型或平台分支带进核心；评审者据此检查每个 Design 与 PR。

## 1. 先修正文档中的几个问题

Issue #65 和早期设计提供了正确的业务边界，但不能逐字翻译成代码。实现以以下判断为准：

| 早期表达 | 本次判断 |
| --- | --- |
| 设备内模块列出 REST/SSE 接口 | 设备内是单进程，默认使用 C++ Port；HTTP/SSE 只存在于跨进程边界 |
| Schedule 和 TimingTask 各自保存后再“同一事务”提交 | 两个领域仍分开，但原子性由应用层的 `CalendarStorePort` 明确承诺，不能靠调用顺序假装事务 |
| MCP Tool 直接编排多个业务模块 | MCP 只做 Schema、路由和结果映射；跨领域编排属于 Application Use Case |
| 提前固定九张 IM 表 | 先稳定通知、投递、动作三类语义与幂等键；表结构由 Gateway 的真实查询和一致性需求推导 |
| `schedule_id` 同时出现整数与字符串 | 跨边界标识统一为不透明字符串；数据库内部主键可以不同，但不能泄漏 |
| 强提醒固定使用临时 SSE | SSE 是候选 Adapter，不是领域事实；只要满足动作投递、重放、过期和确认契约即可替换 |

## 2. 质量目标先于模式名称

模式只是手段。当前架构首先服务五个可验证目标：

| 质量属性 | 场景 | 可验证响应 |
| --- | --- | --- |
| 可迁移 | 微信能力受限，需要改接飞书 | 核心零修改；新增 Adapter、Profile 和契约测试后可运行 |
| 一致性 | 创建日程时设备掉电 | Schedule 与 TimingTask 同时存在或同时不存在 |
| 可恢复 | IM 暂时不可用 | 本地业务提交不丢失；通知进入明确的待重试或降级状态 |
| 可测试 | 开发机没有 ESP32 与外部服务 | 纯 C++ 用内存 Adapter 跑通主要用例 |
| 可追踪 | 用户追问“为什么提醒两次” | `request_id → schedule_id → task_id → event_id → delivery_id` 可关联 |

PR 只说“用了某某模式”不算设计依据，必须说明它改善了哪个场景以及怎么测。

## 3. 从传统架构中借什么

### 3.1 Hexagonal Architecture

借用“核心与外部世界隔离”的思想。每个外部依赖都通过 Port 进入，内存实现与真实实现拥有同一契约，因此开发和迁移不需要先准备全部硬件。

### 3.2 Clean Architecture 与稳定依赖

依赖指向更稳定的策略：Adapter → Application → Domain。领域模型不认识框架、数据库、UI 或供应商 SDK。ESP-IDF 的 `REQUIRES` / `PRIV_REQUIRES` 是这条规则的可执行表达，不只是目录美观。

### 3.3 Microkernel / Plugin

借用“小核心 + 可替换实现”，但不照搬桌面应用的动态插件。ESP32 上使用编译期注册、启动时按 Profile 选择，避免动态加载、ABI 漂移和难以估算的内存开销。

### 3.4 Anti-Corruption Layer

小智、XRobot、Koishi 和各 IM SDK 的消息必须在 Adapter 内转换为 VoiceLife 语义。外部字段变更最多影响一个 Adapter，不得沿调用链扩散。

### 3.5 Strangler Fig

迁移小智时允许新旧实现短期并存：先用 Port 包住旧能力，再逐段替换，不做一次性重写。每迁移一段，都要有相同输入输出的对照测试和可回退点。

## 4. 模块和依赖

```text
voicelife_runtime                  只组装
├── inbound: voicelife_voice, voicelife_mcp
├── outbound: voicelife_linx, voicelife_linx_esp, voicelife_im, voicelife_platform
└── voicelife_application         跨领域用例
    ├── voicelife_schedule        日程事实
    ├── voicelife_timing          调度事实
    └── voicelife_contracts       最小公共类型

语音出站链路再细分一层：`voicelife_linx` 只做 Linx 协议防腐和 Provider，
`voicelife_linx_esp` 才能依赖 ESP-IDF WebSocket/TLS；Runtime 负责选择并组装，
核心 Voice Port 不认识这两个组件的 SDK 句柄。
```

规则：

1. `contracts` 只能放跨边界稳定值，不能变成所有模块都往里扔的 `utils`。
2. Domain 不依赖 Application 或 Adapter。
3. Application 可以协调多个 Domain，但不能出现 HTTP、JSON SDK 或板卡 GPIO。
4. 入站 Adapter 把外部请求映射为 Use Case；出站 Adapter 实现核心要求的 Port。
5. `runtime` 是唯一可以认识所有实现的地方；它不做业务判断。
6. 跨组件只能引用公开 `include/`，不能包含其他组件的 `src/` 或私有头文件。

### 4.1 目录与文件命名

- ESP-IDF 组件统一使用 `voicelife_<capability>`，小写下划线既避免与 IDF/第三方组件重名，也能直接作为 CMake 依赖名。不要使用 `common`、`misc`、`manager` 这类看不出所有权的名字。
- 每个组件的公共头文件放在 `include/voicelife/<capability>/`，路径与 C++ namespace 对齐；私有实现放 `src/`，不从其他组件直接引用。
- C++ 文件使用小写下划线与 `.cc` / `.h`；测试文件使用 `_test.cc`。Python、Shell 和配置文件沿用各自生态的常见后缀，不制造项目专属缩写。
- `tests/host` 放无硬件 C++ 测试，`tests/python` 放构建工具测试；真机测试独立建目录后再接入，不能混在主机测试里伪装通过。
- `docs/adr` 中的文件使用 `NNNN-title.md`，编号只增不改。第三方许可原文放 `third_party/licenses`，迁移清单和上游 commit 统一记录在[第三方来源与许可](../engineering/third-party-notices.md)。

`voicelife_platform` 当前只放内存存储、时钟和标识等轻量出站实现。某类 Adapter 一旦引入独立 SDK、持久化格式或生命周期，就拆成明确组件，例如 `voicelife_storage_sqlite`；不能继续把所有硬件和基础设施塞进 `platform`。

### 4.2 ESP-SparkBot 板级边界

`voicelife_board_esp` 只承载 ESP-SparkBot 的板级事实和设备探针契约：ST7789/ES8311/OV2640/UART1/BOOT 的引脚与总线参数、能力证据状态、GPIO46 功放/背光共享线的逻辑仲裁，以及只读的芯片、容量、MAC 指纹和分区报告。Profile 不暴露 ESP-IDF 句柄；仲裁器不直接写 GPIO；主机调用探针必须返回 `kUnavailable`，不能用模拟数据冒充实板证据。

阶段 A 只把组件接入 `runtime` 的 CMake 装配并锁定主机契约，保持现有语音和 SSD1306 运行时不变。阶段 B 才将 VoiceLife 显示输入适配到小智官方 SparkBot 的 ST7789/LVGL 实现；阶段 C 才让 ES8311 驱动消费音频 Profile 和 GPIO46 仲裁结果。

## 5. Port 设计规则

- 名称描述业务需要，例如 `CalendarStorePort`、`NotificationPort`，不要叫 `IManager` 或 `CommonService`。
- 参数使用值对象或平台无关结构，不传 `cJSON*`、HTTP Request、Koishi Session 或 ESP 句柄。
- 返回明确的 `Status/Result`，错误需要区分无效输入、冲突、不可用和内部错误。
- 超时、幂等、顺序、原子性和所有权写在接口旁。调用方不能靠猜。
- Port 应围绕用例设计，不为某个 SDK 的每个方法做一对一镜像。
- 一个 Port 同时被两个真实 Adapter 需要，或有明确迁移场景时才抽象；不要为假想未来制造空接口。

`SaveScheduleWithTimingTask` 是一个有意保留的粗粒度 Port。它把“必须原子提交”的业务约束交给存储 Adapter，而不是暴露两次 `save()` 再期待调用方处理半成功。

## 6. 适配器 Profile 规范

### 6.1 配置包络

每个设备 Profile 必须通过 `config/adapter-profile.schema.json`，并包含：

```json
{
  "schemaVersion": 1,
  "id": "esp32s3-production",
  "target": "esp32s3",
  "adapters": {
    "audio": { "driver": "xiaozhi-afe", "capabilities": ["wake-word", "aec"] },
    "speech": { "driver": "xrobot-websocket", "capabilities": ["streaming-asr", "tool-call", "tts"] },
    "storage": { "driver": "sqlite", "capabilities": ["atomic-calendar-write", "restart-recovery"] },
    "im": { "driver": "voicelife-gateway", "capabilities": ["notification", "interactive-action"], "configRef": "nvs://im-gateway" }
  },
  "sdkconfig": ["CONFIG_SPIRAM=y"]
}
```

约束：

- Profile 保存选择和非敏感参数，不保存 token、密码、用户标识和私钥。
- `configRef` 只能使用 `nvs://`、`env://` 或 `secret://` 引用。
- `driver` 是稳定注册名；重命名属于兼容性变更，需要迁移脚本或别名窗口。
- `capabilities` 是 Adapter 对外承诺，启动时必须与 Use Case 的必需能力核对。
- 未实现的能力不能为了通过配置而虚报；降级策略要在调用前明确。
- Profile Schema 变更需要版本号、兼容读取规则和 ADR。

### 6.2 不做万能插件基类

Audio、Speech、Storage 和 IM 的生命周期、实时性与错误语义不同。项目只统一 Profile 包络和能力命名，不强迫所有 Adapter 继承一个 `Plugin` 基类。每类 Port 有自己的工厂和契约测试。

### 6.3 注册与选择（目标状态，尚未实现）

每个 Adapter 提供：

1. 稳定的 `driver` 名称；
2. 实现的 Port；
3. 能力集合与资源预算；
4. 配置校验函数；
5. 契约测试套件。

目标 Runtime 在启动阶段完成：读取 Profile → 找到编译期注册的工厂 → 校验配置引用 → 核对能力 → 创建 Adapter → 注入 Use Case。任何一步失败都应停止相关能力并给出可定位错误，不能静默换实现。

当前存储 Adapter 使用编译期 Profile 固定装配，不需要凭据或运行时工厂。加入需要运行时选择、能力协商或凭据引用的真实 Adapter 前，必须先实现对应工厂注册、能力核对和凭据引用解析，并为每个 Port 建立共享契约测试。

## 7. IM 快速适配规则

核心只产生平台无关语义：

```text
NotificationIntent
├── event_id / correlation_id
├── kind: schedule.created | reminder.due | reminder.changed
├── recipient_ref
├── semantic_payload
├── required_capabilities
└── expires_at
```

Gateway 内部按能力选择微信、飞书或其他 Adapter：

| 能力 | 微信示例 | 飞书示例 | 核心降级 |
| --- | --- | --- | --- |
| `plain-text` | 文本/客服消息 | 文本消息 | 必须支持 |
| `rich-card` | 模板或 H5 | 交互卡片 | 降级为文本 |
| `interactive-action` | H5 动作链接 | 卡片按钮 | 降级为语音处理提示 |
| `delivery-receipt` | 视具体通道而定 | 回执事件 | 只记录 accepted，不伪造 delivered |

禁止事项：

- Domain 出现 `wechat_open_id`、`feishu_card`、模板 ID 或平台错误码。
- 用平台展示格式作为业务事实保存。
- Adapter 绕过 TimingTask 直接关闭或推迟提醒。
- 把平台“请求已接受”当成“用户已收到”。

新增平台的最短路径：实现 Gateway Port → 声明能力 → 通过公共契约测试 → 添加部署配置 → 做一个真实通道冒烟测试。核心和设备固件不应因平台变化重新建模。

## 8. 其他模块照同一原则适配

| 模块 | 稳定语义 | 可替换 Adapter | 典型能力 |
| --- | --- | --- | --- |
| Voice | Session、Turn、Generation、Announcement | XRobot、小智协议、未来 Provider | `tool-call`、`cancel-generation`、`proactive-tts` |
| Audio | PCM 帧、播放、采集状态 | I2S Codec、AFE、不同板卡 | `aec`、`full-duplex`、`wake-word` |
| Storage | 原子提交、查询、恢复 | 内存、SQLite/FATFS-WL、未来经验证的块设备 VFS | `transaction`、`restart-recovery`、`capacity-report` |
| Transport | 连接、消息、重连 | WebSocket、MQTT | `ordered-delivery`、`binary-audio` |
| Clock | 单调时间、UTC、时区转换 | 系统时钟、测试时钟 | `sntp-synced`、`monotonic` |

### 8.1 语音子架构：实时数据面与会话控制面分离

语音模块采用“实时音频数据面 + 会话控制面 + Provider 防腐层”。ESP32-S3 的 I2S/AFE、Opus/PCM、有界队列和 WSS 连接属于 Adapter；`VoiceSession` 只处理会话状态、generation、帧序列、打断和稳定事件。Linx 的 hello/listen/stt/tts/abort 字段、小智的全局状态和 ESP-IDF 句柄不得进入核心 Port。

本次骨架实际落下了 `AudioInputPort`、`AudioOutputPort`、`VoiceTransportPort`、`CodecStrategy`、`ASRAdapter`、`TTSAdapter`、`RealtimeAdapter`、`SpeechProviderAdapter`、`SpeechProviderRegistry` 和 `VoiceSession`。Registry 使用编译期工厂，启动时按 Provider ID 和能力集合创建实现；未注册或缺能力直接失败，不静默切换。

这组模式有明确边界：Adapter/防腐层隔离 Linx 与小智，Strategy 选择 PCM/Opus 和会话模式，State 拒绝非法迁移，Factory/Registry 完成 Profile 驱动装配，Observer 产出 `VoiceEvidence`。不引入所有模块共用的万能 `Plugin` 基类，也不在 ESP32 上做动态加载。

Linx 官方 WebSocket 需要 Bearer/Device-Id/Client-Id 鉴权、hello 协商音频参数，支持 OPUS/PCM 二进制帧和 listen/stt/tts/abort 控制消息；具体协议映射和 ESP32-S3 真机验收顺序见 [语音模块子架构](./voice-subarchitecture.md)。

### 8.2 SQLite 连接与业务仓储分离

业务服务只依赖本领域 Repository，不能包含 SQL、SQLite 句柄、PRAGMA 或连接生命周期。当前最小实现按以下方向依赖：

```text
ScheduleService
        -> ScheduleRepository
        -> SqliteScheduleRepository
        -> SqliteDatabase / SqliteStatement
        -> sqlite3
```

`ScheduleRepository` 只表达日程读写能力，不设计跨领域的泛型 `Repository<T>`。SQL 集中放在 SQLite 组件的 `src/sql`，行映射放在 `src/mapping`。SQLite C API 只允许出现在 Database/Statement 实现中，业务服务和 Repository 都不得直接调用。

当前只实现建表、插入和查询，并由主机集成测试验证真实连接。ESP32 Runtime 已组装唯一的 FATFS/WL 数据卷和 SQLite 数据库实例，只执行 Schema 初始化与健康检查，尚未装配日程 Repository；后续领域 Repository 必须共享该实例。四轮实板测试的提交均值中位数约 1.16 秒，已经超过音频实时路径预算；写操作必须离开音频实时任务。

当前唯一通过资格测试的组合是 SQLite 3.53.4、ESP-IDF 6.0.2、FATFS/WL 4 KiB 扇区、`DELETE + EXTRA + psow=0`。`joltwallet/littlefs 1.22.3` 的三组候选配置都出现显式回滚泄漏，不能作为兼容实现保留。生产挂载必须使用 `format_if_mount_failed=false`，失败时保留现场并进入受限模式。

实板证据、Flash 操作规则和未完成验收见 [SQLite 实板验证与 Flash 恢复手册](../engineering/board-storage-validation.md)。

## 9. 数据、一致性与幂等

- 本地 Schedule、TimingTask、Instance、ReminderRule 和 ReminderTrigger 是设备侧权威事实。
- `request_id` 防止命令重放；`event_id` 防止事件重复；`delivery_id` 和 `action_id` 属于 IM 边界。
- ID 跨边界统一为不透明字符串。不要把数据库自增规则暴露给 Agent 或 Gateway。
- 写入先完成本地事实，再发布外部意图。通知失败不能伪装成本地业务失败。
- 内存 Adapter 只用于串联和测试；生产存储必须证明掉电原子性、重启恢复和容量上限。
- 存储实现必须通过目标文件系统/VFS 的同一套实板契约；上游库说明、主机测试或单次 `quick_check=ok` 不能替代故障注入。
- 时间持久化使用 UTC，展示和周期计算显式携带 IANA 时区；单调时钟用于超时，不用于日历时间。

## 10. 安全与隐私

- bearer token 只能通过 HTTPS 发送；当前代码在配置阶段拒绝明文 HTTP。
- 凭据不进 Git、Profile、日志、崩溃转储、MCP ToolResult 或通知语义载荷。
- 外部输入在 Adapter 边界做大小、类型、Schema 和版本校验。
- 日程备注默认视为隐私数据；日志使用标识和状态，不打印全文。
- Adapter 权限按最小能力申请。一个 IM Adapter 不应获得本地数据库直接访问权。

## 11. 测试分层

| 层级 | 必测内容 |
| --- | --- |
| Domain | 时间边界、周期、冲突、状态机；纯 C++、确定性时钟 |
| Application | 原子提交、幂等、跨领域顺序、通知降级 |
| Adapter Contract | 同一 Port 的所有实现跑同一套契约 |
| Architecture | CMake 依赖方向、禁止外部类型进入 Domain |
| Integration | XRobot、存储、Gateway 的协议和错误映射 |
| Hardware | Codec、唤醒、网络重连、掉电恢复、资源预算 |

测试替身优先使用内存 Adapter，不使用大量只验证调用次数的 mock。Mock 适合故障注入，不替代真实契约。

新增行为遵循 Red → Green → Refactor。主机测试按组件拆分并通过 CTest 名称/标签筛选；RED 必须证明缺少的是目标行为，GREEN 只补最小实现，重构期间保持相关测试通过。硬件相关代码按 ESP-IDF 约定补 Unity 测试，连续真机执行再使用 pytest-embedded；主机测试不能替代 Codec、网络和掉电恢复证据。

## 12. 变更与评审清单

引入或更换 Adapter 前，Design/PR 必须回答：

- 哪个真实迁移或质量场景需要它？
- Port 是否泄漏供应商、协议或框架类型？
- 新 Adapter 声明了哪些能力，缺少能力如何降级？
- 配置是否可验证，凭据是否只保留引用？
- 幂等键、超时、重试和错误映射是什么？
- 有无公共契约测试、集成测试和资源预算？
- 旧实现如何并存、切换、回退和最终删除？
- 是否需要 ADR？

## 13. 参考来源

以下资料在 2026-08-03 重新核对，项目只吸收与嵌入式模块单体相关的部分：

- Alistair Cockburn, [Hexagonal Architecture](https://alistair.cockburn.us/hexagonal-architecture/)
- Robert C. Martin, [The Clean Architecture](https://blog.cleancoder.com/uncle-bob/2012/08/13/the-clean-architecture.html)
- Microsoft Azure Architecture Center, [Anti-Corruption Layer](https://learn.microsoft.com/en-us/azure/architecture/patterns/anti-corruption-layer)
- Microsoft Azure Architecture Center, [Strangler Fig](https://learn.microsoft.com/en-us/azure/architecture/patterns/strangler-fig)
- Espressif, [ESP-IDF 6.0.2 Build System](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-guides/build-system.html)
- Espressif, [ESP-IDF 6.0.2 Unit Testing](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-guides/unit-tests.html)
- CMake, [CTest command-line reference](https://cmake.org/cmake/help/latest/manual/ctest.1.html)
- Google, [C++ Style Guide：File Names](https://google.github.io/styleguide/cppguide.html#File_Names)
- GitHub Docs, [Creating a default community health file](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/creating-a-default-community-health-file)
- MADR, [Markdown Architectural Decision Records](https://adr.github.io/madr/)
- Gitmoji, [Gitmoji API](https://gitmoji.dev/api/gitmojis)

不采用 Web 微服务教程里的固定分层数量、通用 Repository 泛型、动态插件加载和“每个实体一个服务”。这些做法没有改善当前质量场景，反而会增加 ESP32 的构建、内存和调试成本。
