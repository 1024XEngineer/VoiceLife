# 单文件规模治理与拆分计划

这份计划把“超过 500 行”变成需要解释和拆分设计的信号，而不是把代码机械切成小块。Runtime 的板级输入、显示投影、语音装配和 Linx 启动协调已经拆出；SSD1306 的文本布局和帧缓冲渲染也已隔离并有主机回归保护，接下来处理仍然超线的会话文件。生成资源、第三方代码和测试大样本不参与同一阈值。

下一步：新功能 PR 从本计划的分级阈值执行；涉及高风险板级代码的拆分，必须先保留现有行为的回归测试和对应 Profile 的回退证据。SparkBot 应实现并列的 ST7789/LVGL 呈现与板级装配，不得复用 VoiceLife PCB 的 SSD1306、GPIO 或 Codec 适配器。

## 为什么不只定一个数字

- ESLint 的 `max-lines` 文档明确说没有客观通用上限，常见建议在 100 到 500 行，其默认值为 300。[ESLint](https://eslint.org/docs/latest/rules/max-lines)
- Checkstyle 的 `FileLength` 默认却是 2,000 行，说明工具默认值是门禁策略，不是可维护性的结论。[Checkstyle](https://checkstyle.org/checks/sizes/filelength.html)
- C++ Core Guidelines 要求函数只做一个逻辑操作、保持短而简单；这比按文件行数切割更接近真正的职责边界。[C++ Core Guidelines F.2/F.3](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rf-logical)
- Google 的评审实践强调可独立理解和回退的小变更，100 行通常合适、1,000 行通常过大，但仍以 Reviewer 判断为准。[Google Engineering Practices](https://google.github.io/eng-practices/review/developer/small-cls.html)

因此，500 行适合作为 VoiceLife 手写生产文件的警戒线，不应成为脱离模块职责的硬性拆分目标。

## 适用范围与阈值

| 范围 | 0-500 行 | 501-800 行 | 超过 800 行 |
| --- | --- | --- | --- |
| 新增手写生产代码 | 正常 | PR 必须说明为何职责仍单一，并在同一 PR 拆分或获架构 Review 豁免 | 不允许新增；先设计拆分 |
| 既有手写生产代码 | 正常维护 | 修改触及第二个职责时，顺带拆出一个稳定边界 | 建立拆分 Issue；后续功能不得继续堆入主文件 |
| 单元/契约测试 | 正常 | 按行为场景拆文件，允许共享 fixture | 以场景或 Port 契约拆分；不按断言数量硬拆 |
| 生成文件、字库、PCM/GIF 资源、第三方代码 | 排除 | 排除 | 排除，必须标注来源或生成方式 |

行数按物理行计数，包含注释和空行。它用于发现文件同时承担过多职责，不用于评价代码作者。

## 当前清单与拆分顺序

| 优先级 | 文件 | 当前规模 | 拆分方向 | 保护条件 |
| --- | --- | ---: | --- | --- |
| P0 | `components/voicelife_runtime/src/runtime.cc` | 1,237 -> 693 | 已拆为 `runtime_linx_bootstrap`、`runtime_board_input`、`runtime_presentation`、`runtime_voice_wiring`；Runtime 暂保留启动编排和交互状态机。后续若再触及第二项职责，拆出 `runtime_interaction_coordinator` | 已保持 GPIO、I2S、Codec 初始化和 SSD1306 默认交互的调用顺序；每个移动步骤都有独立提交。ESP-IDF 构建和旧板真机回归仍待工具链/设备可用时补齐 |
| P0 | `components/voicelife_display_esp/src/ssd1306_status_display.cc` | 801 -> 687 | 已拆出 `display_text_layout` 和 `ssd1306_renderer`；前者锁定 UTF-8/全角标点/滚动字节偏移，后者锁定 128x32 页缓冲的文本与情绪布局。原文件仅保留旧板字形资产、字形选择和 I2C/面板生命周期 | 已新增主机像素位置、5x7 状态栏、情绪图和按字符滚动断言。内置字形表是受保护资产而非通用渲染职责；下一次修改字形资产或驱动序列前，先补旧板实机截图/串口回归证据。不得混入 SparkBot/LVGL |
| P1 | `components/voicelife_voice/src/voice_session.cc` | 539 | 将状态迁移规则与协议事件映射分离，保持 `VoiceSession` 为会话语义所有者 | 每个状态迁移和 generation 隔离的现有契约测试必须不变 |
| P1 | `components/voicelife_runtime/src/linx_ota_bootstrap.cc` | 521 | 拆出设备身份、OTA 配置解析和 ESP 启动协调 | OTA 签名、目标分区与回退流程不能随文件拆分改变 |
| P1 | `services/im-gateway/src/application/services.ts` | 1,754 | 按通知投递、动作执行、幂等/事务协调拆成应用服务；不拆散同一业务事务 | TypeScript 契约、持久化回归与 IM Gateway CI 全量通过 |
| P2 | `services/im-gateway/src/infrastructure/wechat/wechat-official-adapter.ts` | 584 | 按鉴权、消息编码、HTTP 客户端和错误映射拆分 | 保持平台防腐边界，领域层不出现微信字段 |

`font16_data.h` 和 `farewell_pcm.h` 等生成资源不进入拆分列表。它们应在后续资源 PR 中维护 manifest、哈希、来源和 Flash/PSRAM 预算，而不是人工按行切文件。

## 每次拆分的做法

1. 先画出该文件的责任地图：公开入口、硬件/协议边界、状态、资源所有权和已有测试。
2. 选择一个可独立命名的边界，先抽私有实现；不在首次移动时顺便改协议、引脚、渲染语义或业务规则。
3. 在同一 PR 保持公开 API 和调用顺序稳定，新增覆盖被移动边界的测试。
4. 对板级文件，先验证原 Profile；涉及共享 Runtime/Voice/公共 Port 时，运行所有支持 Profile 的适用回归。
5. 用小 PR 逐段提交。仅移动代码的 PR 可以较大，但功能变更必须另开 PR。

## 自动化与例外

下一阶段新增 `scripts/check_file_size.py` 和受版本控制的基线清单：CI 阻止新增手写生产文件超过 500 行，也阻止既有超标文件继续增长；不会因历史债务一次性阻塞全仓库。`runtime.cc` 当前 693 行的例外理由是它仍是唯一的交互状态机协调点，下一次同时涉及启动、队列或证据处理的改动必须先拆出协调器。例外仅限生成物、第三方镜像、自动生成的契约 fixture 或经过架构 Review 的单一表驱动文件，PR 要写明原因、到期迁移计划和测试证据。

不以“低于 500 行”为合并标准。以下情况仍必须拆：一个文件同时拥有 GPIO 初始化、会话状态、协议解析和渲染；一个函数跨越多个业务事务；或任何改动已经无法由 Reviewer 在一个明确场景内理解和回退。
