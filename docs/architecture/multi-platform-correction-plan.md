# VoiceLife 多平台架构修正计划

一句话结论：VoiceLife 当前的领域契约和语音 Port 可以继续使用，但 Runtime 仍绑定 VoiceLife PCB 的 GPIO、显示、按键、唤醒和 Linx 启动流程；在接入第二个平台前，必须先建立统一的平台装配契约和单一交互事件执行边界。

下一步动作：先完成本计划的 MS-A 和 MS-B，形成可编译、可测试的 `PlatformAssembly` 空骨架；经 Design Issue 和 PR Review 通过后，再接入 SparkBot 或其他真实硬件。

本计划已按 ESP-IDF、Zephyr 和 SLSA 的公开规范，以及 VoiceLife 已合入和待审 PR 复核。结论是方向成立，但硬件 Profile 必须是**构建期选择、生成只读 manifest、启动时校验**，而不是让现场运行时从任意 JSON 切换 GPIO 或驱动。

## 1. 依据与范围

本计划以远端 `main@0bc930d` 为基线，覆盖以下问题：

- 多 MCU、多个音频拓扑、不同显示与输入设备的适配方式；
- 编译 Profile 与运行时装配之间的断点；
- Voice、MCP、Schedule、Timing、Storage、IM 的主链连接；
- 硬件事件、Provider 回调和定时器并发进入 Runtime 的一致性；
- 与 `78/xiaozhi-esp32` 官方实现的借鉴边界。

本计划不承诺一次性完成所有硬件适配，也不把小智的板型目录和全局 `Application` 状态机迁入 VoiceLife。

### 1.1 规范复核结论

| 依据 | 对本计划的结论 |
| --- | --- |
| [ESP-IDF Event Loop Library](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/esp_event.html) | ESP-IDF 支持带专用任务的用户事件循环和事件投递；`InteractionEventLoop` 合理，但 handler 不能承担长时间、阻塞的 I/O。 |
| [ESP-IDF FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos_idf.html) | SMP 下共享状态必须正确同步；临界区应短，复杂处理应延后。这支持“输入源只投递，单一循环决策，专用 worker 执行 I/O”。 |
| [Zephyr Board Porting Guide](https://docs.zephyrproject.org/latest/hardware/porting/board_porting.html) 与 [Devicetree 设计目标](https://docs.zephyrproject.org/latest/build/dts/design.html) | 硬件描述应有单一事实源，并按 board/SoC/variant 层次管理。VoiceLife 应采用版本化的板级 Profile 生成描述符，而不是在 Runtime 中硬编码或从不受控文件动态加载。 |
| [Zephyr: Devicetree versus Kconfig](https://docs.zephyrproject.org/latest/build/dts/dt-vs-kconfig.html) | 硬件与启动配置属于板级描述；软件特性是否进入固件属于构建配置。VoiceLife 的 Profile、Kconfig 和 CMake 必须生成同一份构建身份。 |
| [SLSA Build Levels](https://slsa.dev/spec/v1.1/levels) | 固件 manifest 应记录源码提交、Profile、构建工具链、输入摘要和产物哈希，支持重建、现场诊断和错误构建追溯。 |

这些规范不是要求 VoiceLife 改用 Zephyr 或追求完整 SLSA 等级，而是约束当前 ESP-IDF 实现应遵循的设计原则。

## 2. 当前判断

### 2.1 可以保留

- `AudioInputPort`、`AudioOutputPort` 和 `VoiceTransportPort` 的平台边界；
- `VoiceSession` 的 generation、迟到帧拒绝和播放 Flush 语义；
- `AudioBoardProfile` 对 I2S 拓扑、wire sample、PCM 对齐和 Codec 控制的建模；
- Timing 的 `TimingTaskStorePort` 原子操作契约；
- IM 契约、SSE 动作窗口和幂等回写模型；
- 主机测试、架构依赖检查和 Profile 校验脚本。

### 2.2 必须修正

1. **Profile 只影响构建，不参与运行时组装。** 当前 Schema 只验证 `esp32s3`，Runtime 仍固定创建 VoiceLife PCB 音频 Profile、OLED、GPIO48 和四个按键。
2. **Runtime 是硬件编排与业务编排的混合体。** 启动、显示、按键、唤醒、音量、Linx、MCP 和语音状态都集中在一个类中。
3. **交互事件没有唯一执行者。** 按键任务、唤醒回调、Provider 回调和 esp_timer 都能触发状态迁移、音频动作或显示更新。
4. **业务主链没有接入真实 Store。** `ScheduleService` 仍依赖 mock 数据；SQLite、Timing 和 IM 的契约尚未由设备 Runtime 装配。
5. **能力声明不完整。** Profile 只有 audio/speech/storage/im 四类 Adapter，没有描述显示、输入、唤醒、网络、时钟、功耗和 OTA 的存在性、依赖关系与降级规则。

## 3. 目标架构

```text
Versioned Board Profile + Kconfig + CMake
              |
    Build-time descriptor / firmware manifest
              |
  Boot identity and capability verification
              |
       PlatformAssemblyFactory
              |
       PlatformAssembly
       |- AudioInputPort / AudioOutputPort
       |- WakeDetectorPort
       |- UserInputPort
       |- PresentationPort / FeedbackPort
       |- ConnectivityPort / SecretPort / ClockPort
       |- CapabilitySet
              |
       InteractionEventLoop  <---- all hardware/provider/timer events
              |
       VoiceSession + Use Cases
       |- MCP tools
       |- ScheduleService -> ScheduleStorePort
       |- TimingTaskService -> TimingTaskStorePort
       |- IM reporting/action channels
```

### 3.1 分层规则

- **Domain / Use Case** 不依赖 ESP-IDF、GPIO、I2C、显示驱动、WebSocket 客户端或板型名称。
- **Port** 只表达业务或设备能力，不暴露引脚、寄存器、SDK 句柄和 FreeRTOS 类型。
- **Adapter** 实现一个稳定 Port，并声明它提供的能力、资源预算、线程模型和验证状态。
- **PlatformAssembly** 负责按已编译的板级描述符选择 Adapter、检查能力依赖、创建对象、绑定生命周期；业务规则不放在这里。
- **InteractionEventLoop** 是板端交互状态机和显示快照的唯一写入者。其他线程只能投递事件，不能直接改状态或渲染；它只做状态迁移、generation 校验和工作分发，网络、存储、音频编解码等阻塞操作仍在各自 worker 执行。
- **Profile** 是版本化的构建输入，不保存凭据明文。构建脚本将 Profile、目标、能力摘要、源码提交和工具链版本写入只读 manifest；启动时只校验该 manifest、芯片/容量/分区等设备身份，不接受任意 JSON 改写 GPIO、驱动或硬件拓扑。运行时通过 `nvs://`、`secret://` 或平台安全存储解析机密。

### 3.2 显示能力与所有权（点阵屏到图片屏）

当前 `DisplaySnapshot` 已经提供会话阶段、表情、文本角色和 revision，是可保留的语义输入；但 `runtime.cc` 仍把它翻译为 SSD1306 的字符串和滚动偏移，并直接调用 `display_esp::SetEmotion()`。这不适合 240x240 ST7789/LVGL 图片屏：该屏还需要静态图片/GIF、主题、字体、缓存、刷新和预览图片的生命周期。

- 定义 `PresentationPort::Render(const DisplaySnapshot&)`，由 `InteractionEventLoop` 作为唯一提交者；`DisplaySnapshot` 只保留产品语义，禁止包含 `lv_obj_t`、LVGL 动画句柄、SPI/I2C 参数、文件路径或像素缓冲区；
- 显示 Adapter 自己将 `phase`、`mood`、`role` 和文本映射为板级布局、图片或动画。SSD1306 Adapter 可以继续使用单行状态、点阵表情与字符滚动；SparkBot Adapter 可采用 ST7789/LVGL、RGB565、GIF 和图片预览，但不能把其 UI 对象或动画状态反向暴露给 Runtime；
- 为需要图片的产品动作另设有界 `PresentationCommand`（例如 `ShowPreview(asset_id, ttl, request_id)`、`ClearPreview(request_id)`），由 Use Case 通过事件循环投递。`asset_id` 是受 manifest/资源包索引约束的逻辑 ID，不接受网络 URL、任意文件路径或未验证字节流；显示 Adapter 负责校验、解码、缓存、过期和释放；
- `DisplayCapabilities` 至少声明：是否可用、尺寸、色彩/像素格式、文本、静态图片、动画、预览图片、触摸输入、所需 PSRAM、最大帧缓存、刷新预算及降级策略。没有图片能力的板必须把预览动作降级为文本反馈或明确的 `unsupported`，不能静默失败；
- LVGL 或其他图形框架的全部对象创建、动画更新、图片解码和销毁只在 Display Adapter 的专属渲染任务/受控锁上下文中执行。InteractionEventLoop 不持有显示锁，音频采集、播放和 Provider 回调不得直接调用 LVGL；
- 资源与固件版本绑定：每个图片/GIF/字体资源记录来源、许可证、上游 commit、内容哈希、尺寸、解码后峰值内存和加载位置。固件 manifest 记录资源包摘要；资源缺失、哈希不符或内存预算不足时进入可诊断的文本/静态图降级，不能在启动后动态下载替换；
- 小智 SparkBot 的显示实现可以作为 `SparkBotPresentationAdapter` 的上游基线，而不是 VoiceLife 的显示领域模型。移植前必须固定上游 commit 和许可证；移植后记录必要改写、对比截图/视频和 VoiceLife 语义映射，验证后才可声称等价。

## 4. 分阶段修正计划

### 4.0 可恢复基线与同级板型保护（所有 Milestone 的前置门禁）

目标：将当前 VoiceLife PCB 的 `main@0bc930d` 作为首个可恢复金样版本，而非永久冻结的兼容性承诺。VoiceLife PCB 与 SparkBot 是同级板型：两者都可以在各自的 Profile 和 Adapter 中迭代；每一次发布只需能说明相对上一已验证版本改变了什么，并能刷回上一产物排障或恢复。

工作项：


- 将 `main@0bc930d` 记录为 VoiceLife PCB 的首个迁移基线；在对应 Design Issue 中固化板卡版本、Profile ID、已验证固件配置、烧录方式、主机测试结果和真机证据链接。经人工确认后创建 tag 和可复现固件产物；本草案不直接创建 tag 或发布产物；
- 为每个受支持 Profile 建立独立的构建、Profile 校验、启动冒烟和录放音/状态机真机回归清单。`esp32s3-voicelife-pcb-pcm` 与 SparkBot Profile 都可以修改和演进；主机测试只能验证契约，不能替代真机回归；
- 新的 `PlatformAssembly` 先以 `VoiceLifePcbAssembly` 保持当前 PCB 的可用路径，再逐项迁移到通用 Adapter。这个包装是迁移起点，不是冻结旧板的永久实现；每个 PR 只迁移一个边界，禁止在同一 PR 同时改行为、硬件参数和抽象层；
- 对所有受支持板型实行同一发布门禁：改动了哪个 Profile，就验证哪个 Profile 的构建、架构检查、主机测试和对应真机回归。改动共享 Runtime/Voice/公共 Port 时，所有受支持 Profile 均需通过适用回归；
- 每次发布前，保存所有受影响板型的上一已验证固件、manifest、配置摘要和回退步骤。出现启动失败、无声、唤醒异常、状态机回归或资源超预算时，先刷回上一产物恢复可用性，再登记缺陷并定位；
- 新板可先在独立 Profile 和 Adapter 中演进；旧板也可改 GPIO、I2S、Codec 初始化、显示时序和交互默认行为，但必须以新的 Profile/固件版本、行为差异、真机证据和可刷回上一版本的方式交付，不能伪称“未变”；
- 为每次迁移建立板型兼容性矩阵：至少比较构建身份、启动日志、能力集、录音格式、播放首帧、唤醒路径、按键路径、联网重连、空闲功耗和异常恢复。未验证项必须显式标为未知，不能写作“保持兼容”。

完成标志：任一架构 PR 均能回答“影响了哪些 Profile、各自跑过哪些回归、失败时刷回哪个已验证产物”；任何受支持板型都能独立构建、烧录和交付。

### MS-A：平台能力与装配骨架

目标：让同一套 Runtime 可以通过不同 Profile 创建不同平台组合，先不要求真实硬件全部可用。

工作项：

- 将 Profile Schema 从四类 Adapter 扩展为 `platform`、`audio`、`speech`、`storage`、`im`、`display`、`input`、`wake`、`connectivity` 等能力声明，并明确 `board_id`、`board_revision`、`target` 与 Profile schema 版本；
- 定义 `CapabilityId`、能力依赖、必需能力与可选能力的校验结果；
- 新增 `PlatformProfile`、`PlatformAssembly`、`PlatformAssemblyFactory` 接口；
- 在构建时将 Profile、Kconfig、CMake 选择和 Git commit 编译为只读描述符/manifest；启动时对 MCU、Flash、PSRAM、分区和板级探针结果做兼容性校验；
- 将 `VoiceLifePcbEsp32s3Profile()` 从 Runtime 移到 ESP32-S3 Adapter 注册表；
- 用 `Scaffold`、`Headless` 和当前 `VoiceLife PCB` 三个 Profile 跑通组装；
- 将 GPIO、显示、按键、音量和唤醒器配置从 `runtime.cc` 移至板级 Adapter；
- 将当前 `DisplaySnapshot` 的 SSD1306 字符串/滚动翻译移动到 `Ssd1306PresentationAdapter`；第一步以现有行为作为迁移对照，后续 OLED 布局、字体、刷新节奏和表情语义均可在该 Adapter 内独立演进并留存真机对比；
- 为 `DisplayCapabilities`、资源预算和无图片能力的降级路径增加 Profile/主机测试；
- 保留当前主机测试，并新增 Profile 缺能力、冲突能力和不支持目标的失败测试。

完成标志：`main.cc` 只调用 `PlatformAssemblyFactory` 和 Runtime 启动接口；Runtime 中不再出现具体 GPIO 编号、板名或 OLED 驱动调用。

### MS-B：交互事件单写者与设备生命周期

目标：消除多输入源并发修改状态机、显示快照和音频动作的竞态。

工作项：

- 定义 `InteractionEvent`：来源、事件类型、单调时间、session generation、可选文本和追踪 ID；
- 新增有界 `InteractionEventQueue` 与唯一 `InteractionEventLoop` 任务；队列容量、事件类别和满载策略由 Profile/资源预算声明；
- 按键、触摸、唤醒、Provider、VAD、超时和网络状态全部改为投递事件；
- `VoiceInteractionController` 只负责纯状态迁移；事件循环负责执行 transition action，但把网络、SQLite、音频编解码和等待播放完成等可能阻塞的工作投递到各自 worker；
- 显示由 `PresentationPort::Render(DisplaySnapshot)` 接收快照，不允许 Runtime 直接调用具体 OLED 函数；
- 图片/GIF/预览只经有界 `PresentationCommand` 和逻辑 `asset_id` 进入显示 Adapter；渲染框架、文件系统和像素缓冲区不得进入 Voice/Domain 公共接口；
- 为 Display Adapter 指定专属渲染上下文与资源上限；Display Adapter 只消费快照和命令，不能直接触发 `VoiceInteractionController` 状态迁移；
- 明确事件队列满载策略、旧 generation 丢弃策略、停止顺序和资源回收顺序；
- 为乱序事件、迟到事件、队列满载、重复唤醒和定时器回调增加主机测试。

完成标志：任意硬件输入替换只新增 Input Adapter，不修改状态机；同一事件在日志中可追踪到来源和 generation。

### MS-B1：彩屏/图片显示适配（在 MS-A/MS-B 骨架之后）

目标：让点阵屏与图片屏共享会话语义，但各自保留合适的渲染器和资源模型。

工作项：

- 先实现 `Ssd1306PresentationAdapter` 并以当前 VoiceLife PCB 回归证据建立迁移对照；
- 再实现 `SparkBotPresentationAdapter`，将 ST7789/LVGL 初始化、RGB565 flush、主题、字体、GIF 和预览图片限制在该 Adapter；
- 为 SparkBot 资源包建立生成清单，记录逻辑 `asset_id`、来源、许可证、上游 commit、内容哈希、尺寸、解码峰值和 PSRAM/Flash 预算；
- 以 `DisplaySnapshot` fixture 验证两种 Adapter 的语义映射：启动、联网、待机、聆听、思考、播报、错误、长文本、中文/英文混排和无图片降级；图片/GIF 验证以截图、视频、帧率、峰值内存和连续对话稳定性为准；
- 对图片预览定义并发和取消策略：同一 `request_id` 幂等，过期/会话切 generation/内存不足时可取消且释放资源；不允许旧会话图片覆盖新会话状态；
- 显示基础实现采用小智代码时，先完成许可证和上游版本审查。若上游依赖与 VoiceLife 的 ESP-IDF 版本、构建选项或资源分区不兼容，应记录差异并在 Adapter 内最小改写；不得因“原样移植”要求阻断必要的安全、许可证、稳定板保护或架构边界修正。

完成标志：SparkBot 图片/GIF 的加载、刷新、取消与释放不发生在音频实时路径；VoiceLife PCB 与 SparkBot 都通过各自 Profile 构建，并可各自独立演进或回退。

### MS-C：音频与语音 Provider 的多平台适配

目标：把“音频拓扑差异”和“语音服务差异”限制在 Adapter 组合内。

工作项：

- 抽象 `AudioDeviceFactory`，支持外部 Codec Duplex、Direct I2S Simplex、数字麦克风和无音频降级；
- 抽象 `WakeDetectorPort`，支持本地 WakeNet/MultiNet、服务端唤醒和 disabled；
- 将采样率、通道、wire bits、PCM 对齐、DMA、队列深度和资源预算作为 Audio Adapter 的可验证配置；
- 为每个 Speech Provider 定义协商能力、重连、取消、音频格式和 MCP Bridge 版本；
- 保留小智音频数据面的独立任务、有界队列和 generation 失效；不迁移全局 Application、板型宏和 UI 状态；
- 增加“同一 PCM/protocol fixture 在不同 Adapter 上的等价性测试”。

完成标志：新增硬件平台只需新增 Audio/Input/Display/Connectivity Adapter 和 Profile，不修改 `VoiceSession` 或领域服务。

### MS-D：业务持久化与通知主链

目标：把当前主机契约接成设备可恢复的业务路径。

工作项：

- 定义 `ScheduleStorePort`，将创建、更新、取消、查询和撤销的原子边界写清；
- 用 SQLite Adapter 实现 Schedule Store，并复用已有 `StorageTransactionPort` 的 request、deadline、revision 和 health 语义；
- 将 `TimingTaskService` 装配到同一个 Store 生命周期，闭合 occurrence、trigger、outbox 和重试；
- 将 IM reporting/action channel 接入 Runtime 的 Connectivity、Clock、Credentials 和事件循环；
- MCP 工具只调用 Use Case，不直接调用 mock 或设备驱动；
- 增加重启恢复、断网重连、幂等重放、并发更新和存储故障测试。

完成标志：设备重启后日程、提醒状态和未确认通知可恢复；音频任务不持有 SQLite 连接，也不等待存储提交。

### MS-E：第二平台真实适配与验证矩阵

目标：用一个真实的第二平台证明架构扩展成本可控。

工作项：

- 选择 SparkBot 或另一块明确硬件，先完成 Profile、Adapter、能力依赖和硬件验证清单；
- 为无屏、不同显示、不同按键布局、无本地唤醒和不同网络链路定义降级行为；
- 固化每个平台的编译、烧录、启动、录放音、唤醒、重连、OTA 和恢复证据，并随固件输出 `profile_id`、board revision、源码提交、工具链、配置摘要和产物哈希；
- 将硬件测试与主机契约测试分层，禁止用主机测试替代声学、功耗、资源和断电证据；
- 对每个 Milestone 记录新增 Adapter、能力变化、接口变化和未迁移能力。

完成标志：第二平台不修改领域模块和 VoiceSession；新增代码主要位于平台 Adapter、Profile 和验证工具中。

## 5. 建议的接口骨架

以下接口先作为 Design Issue 的实现边界，具体字段在骨架 PR 中定稿：

```cpp
struct PlatformProfile {
    std::string id;
    std::string target;
    CapabilitySet capabilities;
    AdapterSelection adapters;
    ResourceBudget budget;
};

class PlatformAssembly {
public:
    virtual ~PlatformAssembly() = default;
    virtual const CapabilitySet& capabilities() const = 0;
    virtual AudioInputPort& audio_input() = 0;
    virtual AudioOutputPort& audio_output() = 0;
    virtual WakeDetectorPort& wake_detector() = 0;
    virtual UserInputPort& user_input() = 0;
    virtual PresentationPort& presentation() = 0;
    virtual ConnectivityPort& connectivity() = 0;
    virtual Status Start() = 0;
    virtual Status Stop() = 0;
};

class InteractionEventLoop {
public:
    virtual ~InteractionEventLoop() = default;
    virtual Status Start() = 0;
    virtual Status Post(InteractionEvent event) = 0;
    virtual Status Stop() = 0;
};
```

```cpp
struct DisplayCapabilities {
    bool text = false;
    bool static_image = false;
    bool animation = false;
    bool preview_image = false;
    uint32_t max_frame_bytes = 0;
    uint32_t refresh_budget_hz = 0;
};

class PresentationPort {
public:
    virtual ~PresentationPort() = default;
    virtual const DisplayCapabilities& capabilities() const = 0;
    virtual Status Render(const voice::DisplaySnapshot& snapshot) = 0;
    virtual Status Submit(PresentationCommand command) = 0;
};
```

约束：接口只表达能力、受限资源标识和生命周期；不把 GPIO、I2C 地址、FreeRTOS 队列句柄、显示分辨率、`lv_obj_t`、图形框架回调、文件路径、像素缓冲区或具体协议 JSON 放进 Voice 和 Domain 公共头文件。

## 6. 验收标准

### 架构验收

- [ ] 每个受影响 Profile 在迁移 PR 中通过独立构建、Profile 校验和适用主机回归；
- [ ] 每个受影响板型的行为变更有对应真机证据，且与上一已验证版本的能力矩阵差异可追溯；
- [ ] 每个受影响板型的发布产物均保留 manifest、可刷写固件与明确回退步骤；
- [ ] `main.cc` 不包含板型细节；
- [ ] Runtime 不直接引用具体 GPIO、OLED、按键和音频 Profile 工厂函数；
- [ ] Runtime 不直接引用 SSD1306/ST7789/LVGL、显示资源文件或图形框架对象；
- [ ] 每个 Adapter 有明确 Port、能力声明、生命周期和错误语义；
- [ ] Profile 能表达 board identity、目标、Adapter 选择、能力依赖、配置引用和资源预算；
- [ ] 同一 Profile、Kconfig、CMake 选择和固件 manifest 可互相校验；设备不匹配时安全进入受限模式；
- [ ] 至少两个 Profile 可完成主机组装测试；
- [ ] 事件循环是交互状态和显示快照的唯一写入者；
- [ ] 图片/动画能力、刷新率和内存上限由 `DisplayCapabilities` 与资源清单声明；无图片能力时有可测试的降级结果；
- [ ] 显示资源有来源、许可证、上游 commit、内容哈希和固件 manifest 摘要；
- [ ] 显示渲染只在 Adapter 专属上下文运行，音频实时路径和 Provider 回调不触碰图形框架；
- [ ] 领域层不依赖 ESP-IDF 或第三方传输 SDK；
- [ ] 架构演进记录说明每个 Milestone 的边界变化。

### 功能验收

- [ ] 日程创建、查询、修改、取消和撤销通过真实 Store 持久化；
- [ ] Timing occurrence、提醒触发、outbox 和重试可重启恢复；
- [ ] IM 通知与动作命令支持断网重连和幂等重放；
- [ ] Voice Provider 可重连、取消、切 generation，并拒绝迟到音频；
- [ ] 至少一个真实平台完成录音、播放、唤醒或明确的降级验证；
- [ ] 第二平台通过独立 Profile 接入，不修改 Domain、VoiceSession 和 MCP 协议核心。

### 工程门禁

- [ ] 架构检查通过；
- [ ] 主机测试全部通过；
- [ ] Profile 校验全部通过；
- [ ] 每个真实平台有可复现的固件构建和硬件证据；
- [ ] 关键 PR 关联 Design Issue，包含上游参考 commit、改写点、测试和未实现能力。

## 7. 明确不做

- 不复制小智的全量板型目录、显示资源、摄像头和多网络矩阵；
- 不把所有设备差异塞入一个“万能 Board”接口；
- 不让业务服务通过 `if (board == ...)` 判断硬件；
- 不在音频实时任务中执行 SQLite、HTTP 或复杂 JSON 业务处理；
- 不以“主机测试通过”宣称真实声学、功耗、断电恢复或 OTA 已验收；
- 不在平台装配契约未稳定前继续新增第三、第四个平台。

## 8. 风险与决策点

| 风险 | 影响 | 处理决定 |
| --- | --- | --- |
| 不同 MCU 的音频能力差异很大 | Port 过度抽象或运行时失败 | 先定义能力和资源预算，无法满足时显式降级，不用假实现掩盖 |
| Profile 与固件编译选项分裂 | 编译成功但运行时选错 Adapter | 由构建脚本从单一 Profile 生成描述符和 manifest，记录提交/工具链/哈希；启动时只校验，不接受动态改写硬件配置 |
| 架构迁移破坏任一已支持板型 | 已可用硬件出现无声、无法启动或交互退化，且难以定位 | 受影响 Profile 的独立门禁、逐边界迁移、产物留存和真机对比；回归即先恢复上一产物，再定位和修复 |
| 多线程事件导致状态错乱 | 唤醒、按键、TTS 和超时互相覆盖 | 单一事件循环 + generation + 有界队列 |
| SQLite 提交延迟进入实时路径 | 丢帧、卡顿、看似随机的音频故障 | 业务事件进入异步队列，存储只在 Use Case/Store 边界执行 |
| 图片/GIF 和 LVGL 资源挤占内存或阻塞音频 | 彩屏看似可用，但播放卡顿、重连不稳或连续对话崩溃 | 用资源清单和 Profile 声明峰值内存/刷新预算；图形框架在专属任务运行，真机验证峰值内存与连续会话 |
| 图片屏把视觉对象反向耦合进会话 | Adapter 替换时状态机和业务代码被迫改写 | `DisplaySnapshot` 保持语义，`PresentationCommand` 只传逻辑资源 ID；显示不得改变会话状态 |
| 盲目迁移小智实现 | 构建面和维护成本快速膨胀 | 只迁移经 fixture 和真机证据证明的音频/协议能力 |
| 无屏或无唤醒硬件被强行套用当前 UX | 启动失败或假待机 | Capability-driven fallback：无能力即隐藏、替代或禁用对应流程 |

## 9. 与既有 PR 的一致性复核

| PR | 已建立的正确基础 | 对本计划的影响 |
| --- | --- | --- |
| [#92](https://github.com/1024XEngineer/VoiceLife/pull/92) | 已明确 Profile Factory、真实持久化和真实 Adapter 尚未实现，并建立 Ports/Adapters 与架构门禁。 | MS-A 是对该未完成承诺的收敛，不推翻领域边界。 |
| [#106](https://github.com/1024XEngineer/VoiceLife/pull/106)、[#108](https://github.com/1024XEngineer/VoiceLife/pull/108)、[#114](https://github.com/1024XEngineer/VoiceLife/pull/114) | 音频输入/输出 Port、Provider 防腐层、协商格式、固定容量队列和 generation 隔离已有实现与测试。 | MS-B/MS-C 应复用这些契约；不得为适配新板把 GPIO 或 SDK 类型带回 Voice 核心。 |
| [#110](https://github.com/1024XEngineer/VoiceLife/pull/110)、[#112](https://github.com/1024XEngineer/VoiceLife/pull/112) | I2S/Codec/GPIO/DMA 事实被限制在 Audio Profile，且真机证据与“尚未证明的能力”分开记录。 | 这是板级描述符的雏形；应从硬编码工厂函数演进为构建期生成的 Adapter 描述符。 |
| [#122](https://github.com/1024XEngineer/VoiceLife/pull/122)、[#168](https://github.com/1024XEngineer/VoiceLife/pull/168)、[#170](https://github.com/1024XEngineer/VoiceLife/pull/170)、[#172](https://github.com/1024XEngineer/VoiceLife/pull/172)、[#209](https://github.com/1024XEngineer/VoiceLife/pull/209) | Timing、IM、幂等、SSE、存储恢复和 occurrence 契约已有独立演进。 | MS-D 的重点是 Runtime 组装和重启闭环，不是重写这些领域规则。 |
| [#217](https://github.com/1024XEngineer/VoiceLife/pull/217)、[#224](https://github.com/1024XEngineer/VoiceLife/pull/224) | 已提出 Schedule Repository/SQLite 最小纵向链路，并明确 Runtime、更新、取消、撤销尚待接入。 | MS-D 需先将 Store 生命周期与 Use Case 装配完成，再扩大硬件支持范围。 |
| [#222](https://github.com/1024XEngineer/VoiceLife/pull/222)、[#229](https://github.com/1024XEngineer/VoiceLife/pull/229)、待审 [#230](https://github.com/1024XEngineer/VoiceLife/pull/230) | 已把状态、音频、显示和板级控制串成可验证的单板闭环，也暴露出 Runtime 集中编排的扩张压力。 | MS-B 必须先建立事件单写者；#230 不应继续把新板条件和 UI 特例写进同一 Runtime。 |
| 待审 [#232](https://github.com/1024XEngineer/VoiceLife/pull/232) | SparkBot Profile、硬件探针、能力矩阵和 GPIO46 共享电源仲裁方向正确。 | 应修改“官方显示实现必须原样移植且无替代”的绝对表述：上游实现是适配器基线，保留许可证、上游 commit、差异清单与实板等价测试；它仍必须实现 VoiceLife 的 `PresentationPort`，且不能反向改会话状态。 |

结论：此前 PR 的拆分、Port 契约、Profile 事实隔离、真机证据分层和 PR 验证做法符合本计划。当前主要缺口是把这些已存在的独立能力装配成一套可验证的多平台设备 Runtime，而不是重新设计已稳定的领域模块。

## 10. 交付顺序与文档归档

1. 将本计划拆成一个架构 Design Issue，锁定 MS-A/MS-B 的接口和验收标准。
2. 人工确认后建立每个已支持板型的基线 tag、可刷写产物和回归证据索引，并将“受影响 Profile 的回归与回退”加入后续 PR 模板与 CI。
3. 提交空骨架 PR，先评审 `PlatformAssembly`、能力模型和 `InteractionEventLoop`，不混入真实板卡功能。
4. 先将当前 SSD1306 渲染器迁移为 VoiceLife PCB 的 `PresentationPort` Adapter，完成基线对照；再以独立 PR 接入 SparkBot 的彩屏/图片 Adapter 与资源清单。
5. 合并骨架后，分别提交 Runtime 解耦、事件循环、音频工厂和真实 Store PR；每个 PR 都先通过受影响板型的回归与回退门禁。
6. 每个 Milestone 复盘新增能力和边界变化；旧的设计基线不覆盖，只新增演进记录。
7. 第二平台真实适配完成后，再更新 README 的“当前状态”和 Profile 支持矩阵。

本文件是本地审查分支上的修正计划草案，不代表已经批准实施；正式工程决策应以对应 Design Issue 和 Review 结果为准。
