# VoiceLife 本周 PR 代码溯源与术语入门

一句话结论：这周的工作不是“把语音助手一次性做完”，而是沿着架构主干，把 VoiceLife 拆成可替换的语音接口、Linx 协议适配、ESP32-S3 音频探针和 SQLite 存储协议，并用主机测试、固件构建和串口实板数据证明每一层到哪里为止。

一句话动作：先按本文的阅读顺序看代码；遇到术语或某一步的调用关系不清楚，直接带着文件名和行号问，不需要先自己补完背景知识。

## 0. 这份文档看什么

基线是 `2026-08-07` 拉下来的 `main`，目录为 `VoiceLife-latest`，HEAD 为 `f516413`。本周按北京时间周一 `2026-08-03` 到周五 `2026-08-07`，你提交并已合并的 PR 有 9 个：

| 顺序 | PR | 做的事情 | 读代码时要抓住的词 |
| --- | --- | --- | --- |
| 1 | [#92](https://github.com/1024XEngineer/VoiceLife/pull/92) | 建立组件化架构主干、Port 契约、Runtime 组装和测试门禁 | 组件、Port、Adapter、Composition Root |
| 2 | [#104](https://github.com/1024XEngineer/VoiceLife/pull/104) | SQLite 实板故障验证，以及统一存储控制面协议骨架 | 事务、幂等、单写者、FATFS/Wear Levelling |
| 3 | [#106](https://github.com/1024XEngineer/VoiceLife/pull/106) | 建立可插拔语音会话和 Provider 契约，接入 Linx 协议层 | Provider、Registry、ASR、TTS、generation |
| 4 | [#108](https://github.com/1024XEngineer/VoiceLife/pull/108) | 接入 ESP32-S3 的 Linx WSS/TLS Transport、事件队列和分片重组 | WSS、TLS、WebSocket frame、bounded queue |
| 5 | [#110](https://github.com/1024XEngineer/VoiceLife/pull/110) | 为不同 ESP32-S3 板型建立 Audio Board Profile 和板级探针 | I2S、GPIO、DMA、Codec ACK |
| 6 | [#112](https://github.com/1024XEngineer/VoiceLife/pull/112) | 把 `voicelife-pcb` 区分成纯 I2S PCM Profile | simplex、wire slot、PCM 对齐、削波 |
| 7 | [#114](https://github.com/1024XEngineer/VoiceLife/pull/114) | 把 Profile 接进 PCM Audio Port、采集/播放任务和有界帧队列 | Audio Port、period、frame assembler、回退 |
| 8 | [#117](https://github.com/1024XEngineer/VoiceLife/pull/117) | 删除 README 中过时的“为什么重建主干”说明 | 文档清理 |
| 9 | [#151](https://github.com/1024XEngineer/VoiceLife/pull/151) | 固定工程文档入库和 Issue/PR 归档边界 | Refs、文档寿命、研究归档 |

注意：#110、#112、#114 是堆叠关系。后一个 PR 的网页 diff 可能把前一个 PR 的文件也列出来，所以不要只看“文件列表”，要看 PR 目标和最后合入的主干状态。

## 0.2 九个 PR 逐个看：改了什么，设计上解决什么

下面每个 PR 都按三个问题读：**修正了哪里？为什么这样设计？证据到哪里为止？** “设计”不是漂亮的目录图，而是模块边界、依赖方向、失败行为和可替换点。

### PR #92：先把总架构主干接起来

**修正了哪里：** 把原来容易互相引用的语音代码拆成组件，定义公共 Port、状态/结果类型、Runtime 入口和主机测试门禁。以前如果每块板、每家语音服务都直接写进会话逻辑，后续会变成“换一个硬件就改一大片”。

**做了什么设计：** `voicelife_voice` 只描述语音核心需要的能力；硬件、网络和协议实现向它提供 Adapter。`voicelife_runtime` 作为 Composition Root 负责组装，模块依赖向核心接口收敛，而不是让核心依赖 ESP-IDF 或某家云服务。

**证据边界：** 这一 PR 证明的是骨架可以编译、接口可以串联、测试可以卡住越界依赖；不是证明语音已经能在板上完成闭环。

### PR #104：先验证存储路线，再定义数据库边界

**修正了哪里：** 增加 ESP32-S3 SQLite 实板故障探针、数据/OTA 备份恢复流程，以及 `voicelife_storage_sqlite` 的统一存储协议。之前只有“要用 SQLite”的方向，没有证据说明目标 Flash、文件系统和复位场景是否可靠。

**做了什么设计：** 业务模块不拿 `sqlite3*`，也不提交任意 SQL，而是通过 `StorageTransactionPort` 发送结构化的读请求和命名写语句。一个 `StorageWriteRequest` 可以把日程、提醒、幂等记录和 Outbox 放进同一笔事务；底层使用单连接/单写者，避免多个实时任务同时写 Flash。

**证据边界：** 四轮实板实验验证了 `SQLite 3.53.4 + FATFS/Wear Levelling`、回滚、原子提交、重开和 EN 复位恢复；正式 Schedule/Timing SQLite Adapter、Schema migration 和 Runtime 接入仍未完成。

### PR #106：把语音会话从具体服务中拔出来

**修正了哪里：** 以前 Runtime 里的旧 Coordinator/Scaffold 路径没有真正使用新的会话架构，Provider 也容易被硬编码。这个 PR 将 Runtime 改成从 `SpeechProviderRegistry` 创建 Provider，再构造 `VoiceSession`。

**做了什么设计：** `VoiceSession` 只负责会话状态、收音/播放、事件回调和打断；Provider 负责某一套语音服务的完整语义；Registry 负责按名称找到工厂。Provider 通过能力声明告诉 Registry 自己支持什么，不能把尚未实现的 Opus 能力写进声明里。

**后续修正也属于这个设计：** 打断时先递增 `generation` 再 abort/flush；连接、采集和停止失败要进入明确的失败状态；音频帧增加大小上限；Linx 事件校验 `session_id`，避免旧会话消息污染当前会话。

**证据边界：** 主机契约测试证明状态机和错误路径可验证；默认 Runtime 仍使用 `scaffold`，真实 Linx 云端、ASR/TTS 和声学闭环不在本 PR 的完成范围内。

### PR #108：把 Linx WSS 传输接进来，但隔离 ESP-IDF 回调

**修正了哪里：** 增加 ESP32-S3 的 WSS/TLS Transport、凭据解析边界、WebSocket 事件队列和分片重组。网络库回调如果直接操作 VoiceSession，会把网络线程、协议解析和会话状态绑死，也容易在回调里做太重的工作。

**做了什么设计：** `EspWebSocketTransport` 实现 Linx 的 Transport Port；ESP-IDF 回调只封装事件并放入有界 FreeRTOS 队列，worker 再分发文本和二进制数据。文本交给 Linx JSON Codec，分片先由 `WebSocketFragmentAssembler` 拼成完整消息，连接错误、队列溢出和超时都有显式状态。

**证据边界：** WSS/TLS 构建、主机协议测试、事件队列和分片逻辑有证据；真实云端凭据、服务端 ASR/TTS 和网络条件下的长期运行仍待后续验收。

### PR #110：用 Profile 和探针回答“这块板到底是什么”

**修正了哪里：** 不再把某块旧 Codec 板的 GPIO、I2C 地址和音频参数当成所有 ESP32-S3 板的默认值。新增 `AudioBoardProfile` 校验和 `Esp32s3AudioProbe`，区分外部 Codec 板和实际接入的板。

**做了什么设计：** 板级事实放进 Profile：GPIO、I2S 控制器、采样率、通道、wire slot、DMA 和能力；Runtime 选择 Profile，探针只负责验证硬件边界。业务层不应该出现 `GPIO_NUM_x` 这样的板级细节。

**证据边界：** 探针能证明 I2C ACK、I2S 通道生命周期和有限数字读写；它不能证明 Codec 已初始化、扬声器有可听声音或云端语音已闭环。

### PR #112：承认实际板是纯 I2S，重新定义 PCM 对齐

**修正了哪里：** 实板报告的是 `voicelife-pcb / NoAudioCodec`，不能继续复用 Lichuang Codec Profile。新增 `direct-i2s-simplex` 类型，分别描述 RX/TX、GPIO、采样率、wire slot 和 PCM 移位。

**做了什么设计：** 把“总线上传多少位”和“交给语音层的 PCM 是多少位”分开。当前采集端是 32-bit wire slot 转 16-bit PCM，播放端也有自己的移位规则；Profile 负责保存这些硬件事实，PCM 组装器不猜参数。

**为什么这样改：** 同一块板对照 `>>12` 与 `>>14`，削波率从约 `79791 ppm` 降到 `208 ppm`。这是实测出来的配置，不是凭经验填的常数。

**证据边界：** 证明了数字 PCM 有变化、削波受控、有限回放可执行；没有证明模拟 Codec、AFE、AEC、Opus 或声学效果。

### PR #114：把 PCM 探针变成 VoiceSession 可调用的 Audio Port

**修正了哪里：** 将 Profile 接入 Runtime，新增硬件 period 到传输帧的组装器、采集/投递/播放任务和有界队列；同时收紧 Linx Transport 生命周期和会话失败回滚。

**做了什么设计：** `VoiceSession` 不直接调用 I2S，而是拿到 `AudioInputPort` 和 `AudioOutputPort`。采集任务负责从 I2S 得到硬件 period，组帧器负责拼成传输帧，投递任务再交给会话；播放方向由输出队列和播放任务消费。这样硬件节拍、网络帧长和会话状态不共用一个易混淆的字段。

**失败设计也很重要：** Provider 连接成功但后续打开端口失败时要断开回滚；`BeginCapture` 的启动和回滚都失败时进入 `kFailed`；停止采集失败不能伪装成 `kStopped`。失败路径不留半开连接、半开采集或残留回调。

**代码结构设计：** 将过大的源文件拆成平台无关入口、I2S runtime、WebSocket impl/事件处理和 JSON reader，并同步测试 CMakeLists。拆分不是为了少几行，而是让每个文件只承担一个可审查的边界。

**CI 修正：** PR 的最后一个提交 `1fa604e` 使用了 `🔧 fix(timing)`，违反提交规范中“`🔧` 只能搭配 `build/chore`”的规则，导致历史 `主机测试与架构边界` job 在“检查提交描述”阶段失败。正确写法应是 `🔧 build(timing): ...`。这属于提交流程问题，不是 Audio Port 运行逻辑问题；PR 已合并，当前 `main` CI 已通过。

**证据边界：** 主机 27/27、Doxygen、代码规模门禁、ESP-IDF 构建和纯 I2S 实板回退有证据；真实 Linx 云端、ASR/TTS 和可听声学闭环仍未完成。

### PR #117：删除过时说明，不改变系统行为

**修正了哪里：** 删除 README 中“为什么重新搭主干”的阶段性历史说明，让 README 从当前使用和架构信息开始。

**做了什么设计：** 这是文档生命周期设计，不是产品代码设计：已经过时的过程背景不继续伪装成当前使用指南，历史原因保留在 Git/Issue/PR 中。

**证据边界：** 只验证 README 的删除范围和章节衔接，不影响编译、运行或接口。

### PR #151：规定哪些文档进主仓库，哪些留在 Issue

**修正了哪里：** 新增文档入库边界，明确稳定架构、硬件验证和长期维护说明进入仓库；原始研究、过程材料和 Review 讨论留在 Issue/PR。

**做了什么设计：** 给文档定义生命周期和追溯关系：用 `Refs` 连接 Issue/PR，堆叠 PR 按最终合入状态理解，研究结论先提炼成可验证的稳定文档，再入库。后续删除 `research` 目录的 PR #186 就是按这条规则执行的。

**证据边界：** 这个 PR 只建立流程规则，不修改语音、音频或数据库实现。

### 把 9 个 PR 串成一条演进线

```text
#92   定义总架构、Port 和 Runtime 组装点
  -> #106 定义 VoiceSession、Provider、Registry
  -> #108 接入 Linx WSS/TLS Transport
  -> #110 定义板型 Profile 和硬件探针
  -> #112 为实际纯 I2S 板建立独立 Profile
  -> #114 把 PCM 探针接成 VoiceSession 可用的 Audio Port

#104 另一路建立 SQLite 存储协议和实板资格基线
#117、#151 负责 README 和工程文档的生命周期边界
```

这条线的核心不是“PR 越多功能越多”，而是每个 PR 都把一个不稳定的具体问题收敛到一个可替换、可测试、可追溯的边界里。

### 0.1 你目前真正卡住的不是单个术语

从你前面问的 scaffold、Port、Adapter、Provider、Registry、generation、I2S 和数据库来看，你主要混在了四组不同的问题里：

1. **代码放在哪里，和代码扮演什么角色。** `voicelife_voice`、`voicelife_linx` 这些是目录和编译模块；Port、Adapter、Provider、Registry 是这些模块里的架构角色。两者不是同一种分类。
2. **对象怎么装起来，和装好后怎么调用。** Runtime/Registry 负责创建对象；真正说话时，`VoiceSession` 才会反复调用 Provider、Audio Port。Registry 不会经手每一帧音频。
3. **发命令，和搬音频。** `hello`、`listen`、`abort` 属于控制面；麦克风 PCM 和云端 TTS 音频属于数据面。它们可能共用一条 WSS 连接，但对延迟、队列和错误处理的要求不同。
4. **接口已经存在，和产品已经跑通。** 空骨架、主机契约测试、ESP-IDF 构建、实板探针、生产闭环是五种不同证据。看到类名和 `PASS`，不能直接推导出云端语音、真实扬声器或业务数据库已经接入。

读后面的内容时，先问一句：“我现在看的是模块、角色、组装过程、运行调用，还是完成度证据？”先把层次定下来，很多术语就不会互相打架。

## 1. 先看全局：程序从哪里开始

设备固件的入口只有一处：[`main/main.cc`](../../main/main.cc#L10)。`app_main()` 调用 [`runtime::Start()`](../../components/voicelife_runtime/src/runtime.cc#L234)，失败就打印错误，成功只打印“架构主干已启动”。这里的 Runtime 不是某个外部框架的专有名词，而是 `voicelife_runtime` 组件里的启动/组装代码；`runtime::Start()` 就是这套固件的 Composition Root（组合根）。

`runtime::Start()` 里面做三件事：

1. 从 `SpeechProviderRegistry` 创建一个 Provider。
2. 用音频输入端口、音频输出端口和 Provider 构造 `VoiceSession`。
3. 调用 `VoiceSession::Start()`，然后根据编译配置运行板级探针或 Audio Port smoke。

对应代码在 [`runtime.cc`](../../components/voicelife_runtime/src/runtime.cc#L152) 到 [`runtime.cc`](../../components/voicelife_runtime/src/runtime.cc#L220)。

这里有一个容易误会的事实：最新 `main` 默认仍注册并创建 `scaffold` Provider（[`runtime.cc`](../../components/voicelife_runtime/src/runtime.cc#L154)）。Linx Provider、ESP WSS Transport 和真实 PCM Port 已经分别写好并通过契约/构建/实板证据验证，但默认 Runtime 还没有完成“按 Profile 自动选择并装配真实 Linx Provider”的最后一步。所以现在的状态是：**架构和适配器已经在仓库里，默认启动路径仍是骨架实现**。

`scaffold` 的字面意思是“脚手架”。在这里，它不是另一家语音厂商，也不是一个完整语音服务，而是一个最小占位实现：它遵守 Provider 接口，让 Runtime、`VoiceSession` 和构建链路能先接起来。就像装修前先搭出承重结构，后续再把占位件换成 Linx。它能证明“接口可以连接、程序可以启动”，不能证明“已经连上云端并完成 ASR/TTS”。

## 2. 语音模块：从会话到网络，再到板子

### 2.1 先分两个平面

语音代码把事情分成两层。为了不把“创建对象”和“调用对象”混成一件事，控制面分成启动组装和运行调用两步：

```text
启动组装：Runtime -> Registry -> Provider
          Runtime -> VoiceSession(音频 Ports + Provider)

运行控制：VoiceSession -> Provider -> Transport -> 云端
          云端 -> Transport -> Provider -> VoiceSession -> 事件

数据面：I2S 麦克风 -> AudioInputPort -> PCM 帧队列 -> Transport -> 云端
       云端 -> Transport -> CodecStrategy -> AudioOutputPort -> I2S 扬声器
```

`hello`、`listen.start`、`listen.stop`、`abort` 和错误/状态事件属于运行控制；它们可以等待连接、处理超时、重连和错误。数据面不能被 SQLite、业务逻辑或网络阻塞，否则麦克风采集会丢帧。

### 2.2 Port、Adapter、Provider 分别是什么

- **Port（端口）**：核心代码定义的接口，像插座。核心只规定“需要什么能力”，不写 ESP-IDF 或某家云服务的细节。例子是 [`AudioInputPort`](../../components/voicelife_voice/include/voicelife/voice/voice_ports.h#L25)、[`AudioOutputPort`](../../components/voicelife_voice/include/voicelife/voice/voice_ports.h#L52) 和 [`VoiceTransportPort`](../../components/voicelife_voice/include/voicelife/voice/voice_ports.h#L76)。
- **Adapter（适配器）**：把某个具体世界接到 Port 上。`Esp32s3PcmAudioPorts` 是硬件 Adapter，`LinxSpeechProviderAdapter` 是协议 Adapter，`EspWebSocketTransport` 是 ESP-IDF 网络 Adapter。
- **Provider**：一套完整的语音服务实现，负责把传输、JSON 协议、ASR/TTS 事件和音频帧拼起来。`SpeechProviderAdapter` 的公共契约在 [`voice_ports.h`](../../components/voicelife_voice/include/voicelife/voice/voice_ports.h#L166)。
- **Registry（注册中心）**：用字符串和工厂函数保存 Provider。Runtime 通过 `registry.Create("scaffold", ...)` 创建对象，不需要知道具体类的构造细节。

这样做的结果是：换 Linx、换另一家语音服务或换一块板，主要替换 Adapter；`VoiceSession` 不需要认识 WebSocket 句柄、JSON 字段或 GPIO。

你之前说“Port 有点像 Java 的继承”，方向接近，但更准确地说，Port 像 Java 的 `interface`：它只写方法签名和语义，不提供某家厂商或某块板子的具体做法。Adapter 使用 C++ 继承实现这个接口，然后由 Runtime 把实现对象注入需要它的核心对象。这里真正重要的是**依赖反转**：不是 `VoiceSession` 去依赖 Linx 和 ESP-IDF，而是 Linx、ESP-IDF 的实现来满足 `VoiceSession` 定义的接口。

#### 模块和这些角色是什么关系

```text
voicelife_voice
  定义语音核心语义：VoiceSession、Audio Port、Transport Port、Provider 契约、Registry

voicelife_linx -> voicelife_voice
  理解 Linx 的 hello/listen/abort、JSON、ASR/TTS 事件
  用 LinxSpeechProviderAdapter 实现 voice 定义的 Provider 契约

voicelife_linx_esp -> voicelife_linx -> voicelife_voice
  使用 ESP-IDF 建立真实 WSS/TLS 连接
  用 EspWebSocketTransport 实现协议层需要的 Transport Port

voicelife_audio_esp -> voicelife_voice
  使用 ESP-IDF I2S、DMA 和 GPIO 驱动板上音频
  用 Esp32s3PcmAudioPorts 实现 AudioInputPort / AudioOutputPort

voicelife_runtime
  位于组装点：选择实现，创建对象，把上述模块接在一起
```

所以，`voicelife_audio_esp` 不是 Port，`voicelife_linx` 也不是 Provider。它们是模块；模块里面可以包含一个或多个 Port、Adapter、Provider。好比“售后部”是部门，接线员、维修工程师、工单管理员是角色，不能把部门名和岗位名放在同一层比较。

#### 到底有没有固定调用顺序

有两条顺序，要分开看。

**启动时的创建/组装顺序：**

```text
Runtime
  -> 向 Registry 注册 Provider 工厂
  -> Registry 根据名字创建 Provider
  -> Runtime 创建音频输入/输出 Adapter
  -> Runtime 把 AudioInputPort、AudioOutputPort、Provider 注入 VoiceSession
  -> VoiceSession::Start()
```

**开始说话后的运行顺序：**

```text
上行：VoiceSession -> Provider -> Transport -> 云端
下行：云端 -> Transport -> Provider -> VoiceSession -> AudioOutputPort
```

Registry 主要参与“找到工厂并创建 Provider”这一步。对象创建好后，它就退场了。Provider 负责语音服务语义，Transport 只负责网络收发，Audio Port 只负责板上录放。它们不是一条永远固定的 `Port -> Adapter -> Provider -> Registry` 流水线。

### 2.3 `VoiceSession` 到底管什么

看 [`voice_session.cc`](../../components/voicelife_voice/src/voice_session.cc#L51)：

1. `Start()` 让 Provider 连接，拿到协商后的输入/输出音频格式，再打开输入和输出端口。
2. `BeginCapture()` 把状态切到 `kCapturing`，发送 `listen.start` 一类的控制消息，并让输入端口开始投递帧。
3. `HandleInputAudio()` 给每一帧加上当前 `generation` 和递增序号，再交给 Provider 上传。
4. Provider 收到云端事件后回调 `HandleEvent()`；收到 TTS 音频时走 `HandleAudio()`，最后推给 `AudioOutputPort`。
5. `Interrupt()` 先递增 `generation`，再让 Provider abort、刷新播放队列。旧代次的迟到帧会被拒绝。

`generation` 可以理解成“这一轮会话的版本号”。打断时不可能立刻让所有任务停止，网络和 DMA 里可能还有旧数据；版本号让旧数据即使晚到，也无法污染新一轮会话。相关检查在 [`voice_session.cc`](../../components/voicelife_voice/src/voice_session.cc#L144) 和 [`voice_session.cc`](../../components/voicelife_voice/src/voice_session.cc#L355)。

具体看一遍就更直观：

```text
generation = 5：上一轮 TTS 正在播放
用户打断：VoiceSession 把 generation 增加到 6，并清理旧播放队列
网络里迟到一帧：这帧仍带 generation = 5
队列检查：5 != 6
结果：拒绝旧帧，不让上一轮声音混进新一轮
```

因此 generation 不是音频格式、数据库版本或自动递增 ID。它是处理异步迟到数据的“会话代次”。序号解决同一代里的帧顺序，generation 解决“这帧还属不属于当前这一轮”。

`BoundedAudioFrameQueue` 是有界队列：容量写死，满了按策略丢帧或拒绝，不允许无限堆内存。它还检查 `generation`，见 [`audio_frame_queue.cc`](../../components/voicelife_voice/src/audio_frame_queue.cc#L9)。

## 3. WSS 是什么，以及 #108 实际做了什么

### 3.1 WSS 的白话解释

**WebSocket** 是一种可以长期保持连接、双方随时发消息的网络协议。普通 HTTP 通常是“请求一次、返回一次”；语音对话需要服务器随时推送 ASR 文本、TTS 音频、错误和打断事件，所以 WebSocket 更合适。

**WSS** 就是 `WebSocket Secure`：WebSocket 外面套一层 TLS 加密。网址通常以 `wss://` 开头，类似 HTTPS 之于 HTTP。TLS 负责两件事：加密传输、防止中间人冒充服务器。代码在 [`esp_websocket_transport.cc`](../../components/voicelife_linx_esp/src/esp_websocket_transport.cc#L54) 拒绝不安全的 `ws://`，除非显式打开测试开关。

### 3.2 一次 WSS 连接的代码路线

1. `EspWebSocketTransport::Connect()` 解析 `wss://`，从 `SecretResolverPort` 取 token，组装 `Authorization`、`Device-Id` 和 `Client-Id` 请求头（[`esp_websocket_transport.cc`](../../components/voicelife_linx_esp/src/esp_websocket_transport.cc#L71)）。
2. 创建 ESP-IDF WebSocket client，开启证书校验，注册 `WEBSOCKET_EVENT_ANY`（[`esp_websocket_transport.cc`](../../components/voicelife_linx_esp/src/esp_websocket_transport.cc#L85)）。
3. ESP-IDF 回调不直接碰会话，而是先把连接、数据、断开、错误事件放进 FreeRTOS 队列（[`esp_websocket_transport.cc`](../../components/voicelife_linx_esp/src/esp_websocket_transport.cc#L243)）。
4. 工作任务从队列取事件。文本消息交给 Linx JSON codec，二进制消息交给音频回调（[`esp_websocket_transport.cc`](../../components/voicelife_linx_esp/src/esp_websocket_transport.cc#L283)）。
5. WebSocket 消息可能被拆成多个 frame；`WebSocketFragmentAssembler` 先拼完整，再交给上层。队列满时明确报错并把连接标成失败，不静默吞掉。

### 3.3 Linx JSON 做了什么

[`linx_json_codec.cc`](../../components/voicelife_linx/src/linx_json_codec.cc#L104) 负责生成 `hello`、`listen.start`、`listen.stop` 和 `abort`；[`DecodeText()`](../../components/voicelife_linx/src/linx_json_codec.cc#L206) 负责把服务端 JSON 映射成稳定的 `hello`、`stt`、`tts`、`error` 事件。

`hello` 不是打招呼那么简单，它是音频格式协商：服务端返回的 codec、采样率、通道数和帧时长必须和当前配置一致。`LinxSpeechProviderAdapter` 在收到 hello 后保存协商结果，格式变化且没有重配置策略时直接报错，而不是让播放端猜格式（[`linx_speech_provider.cc`](../../components/voicelife_linx/src/linx_speech_provider.cc#L260)）。

## 4. 板级音频：I2S、PCM、DMA 和 Profile

### 4.1 这些名词分别指什么

- **PCM**：未压缩的音频采样。代码里的 `PCM S16LE` 是“每个采样 16 位、有符号、小端序”。它适合在设备内部先验证数字音频是否真的流动。
- **I2S**：芯片和麦克风/扬声器之间传数字音频的串行总线。`BCLK`、`WS/LRCK` 和 `DIN/DOUT` 是它的时钟和数据线。
- **wire slot**：I2S 总线上实际传输的位宽。当前板子是 32-bit wire slot，但最后转成 16-bit PCM，所以需要明确的移位规则。
- **DMA**：让 I2S 外设和内存自动搬运数据，CPU 不必每个采样都亲自复制。DMA 描述符和 frame 数量决定缓冲深度。
- **Codec**：音频编解码芯片，例如 ES8311/ES7210。它负责模拟麦克风/扬声器和数字 I2S 之间的转换。纯 I2S 板没有这些 Codec 控制器。
- **Profile**：一份板型配置，不是业务逻辑。它描述 GPIO、I2S 控制器、采样率、wire slot、PCM 对齐、DMA 和能力。配置样例见 [`esp32s3-voicelife-pcb-pcm.json`](../../config/profiles/esp32s3-voicelife-pcb-pcm.json#L1)。

### 4.2 #110、#112、#114 的连续关系

**#110：先证明“板子是哪一种、数字总线能不能跑”。**

- `AudioBoardProfile::Validate()` 拒绝无效 GPIO、冲突引脚、非法 PCM 位宽和错误拓扑（[`audio_board_profile.cc`](../../components/voicelife_audio_esp/src/audio_board_profile.cc#L57)）。
- 同时保留两类板：带外部 Codec 的 `esp32s3-lichuang`，以及之后发现的 `voicelife-pcb` 纯 I2S 板。
- `Esp32s3AudioProbe::Run()` 依次验证 I2C ACK、创建 I2S RX/TX 通道、初始化标准模式、启动通道、读写有限 PCM 数据（[`esp32s3_audio_probe.cc`](../../components/voicelife_audio_esp/src/esp32s3_audio_probe.cc#L104)）。

**#112：发现实际接入的板不是 Codec 板，单独建纯 I2S Profile。**

`esp32s3-voicelife-pcb-pcm` 的关键值是：采集端 I2S1，GPIO `5/4/6`，16 kHz，32-bit wire slot，PCM 右移 14；播放端 I2S0，GPIO `15/16/7`，24 kHz，32-bit wire slot，PCM 左移 16（[`audio_board_profile.cc`](../../components/voicelife_audio_esp/src/audio_board_profile.cc#L165)）。

为什么不是沿用旧的右移 12？实板对照显示 `>>12` 削波率约 `79791 ppm`，`>>14` 降到 `208 ppm`，同时样本仍然在变化。这个改动是测出来的，不是“看起来合理”就写进去。

**#114：把底层 I2S 探针变成可被会话使用的 Audio Port。**

它新增了 PCM period 到传输帧的组装器、独立采集/投递/播放任务和有界队列。运行时在配置开关打开时执行 `RunAudioPortSmoke()`：采集 300 ms、停止采集、推入一帧测试音、检查捕获帧/播放帧/丢帧/短读写/最低空闲堆（[`runtime.cc`](../../components/voicelife_runtime/src/runtime.cc#L28)）。

### 4.3 串口上看到的数字应该怎么读

实板验证文档记录了 #112 的一轮结果：`pcm_samples=4800`、`bus_read=19200`、`bus_write=960`、`replay=960`、`changed=4716`、`saturation_ppm=208`、`min_heap=369528`（[`esp32-hardware-validation.md`](./esp32-hardware-validation.md#L61)）。

这些数字能证明：I2S DMA 通道启动了，确实读到了非零且变化的数字 PCM，也做了一次有限回放。它们不能证明：Codec 已初始化、扬声器真的发出可听声音、WSS 已连上、ASR/TTS 已闭环。

## 5. 数据库：从一句语音指令到 Flash

先给结论：#104 完成的是**存储路线的实板资格验证，加上一套供领域 Adapter 使用的事务协议骨架**。它还不是“日程已经写进 SQLite”的生产实现。理解这一节时，要始终区分数据库基础概念、目标架构和当前完成度。

### 5.1 数据库不是“一个保存文件的类”

数据库负责长期保存结构化事实。设备重启后，内存里的对象会消失，但日程和提醒不能消失，所以最终要落到 Flash。

几个基础词先分清：

- **表（table）**：同一类数据的集合。例如 `schedule` 保存日程，`timing_task` 保存何时触发。
- **行（row）**：一条具体记录。例如“明天下午三点开会”。
- **索引（index）**：为了按 ID、时间等条件更快找到行而建立的辅助结构。表和索引必须一致。
- **事务（transaction）**：一组写入要么全部成功，要么全部撤销。不能只创建日程，却没创建对应提醒。
- **Schema**：表、字段、索引和约束的结构定义。以后结构变化需要 migration（迁移），不能直接假定旧设备上的数据库天然符合新代码。
- **SQLite**：运行在设备进程里的数据库引擎，不是远端数据库服务器。代码通过 SQLite 读写一个数据库文件。
- **VFS / FATFS / Wear Levelling / Flash**：SQLite 再往下的物理链路。VFS 把 SQLite 的文件操作接到 FATFS；FATFS 管文件；Wear Levelling 把频繁写入分散到不同 Flash 扇区；Flash 才是最终存储介质。

所以“用了 SQLite”并不等于“断电一定安全”。数据库引擎、journal 模式、同步级别、文件系统、VFS 和 Flash 行为必须作为一个整体在目标板上测试。

### 5.2 一条真实业务指令会怎么走

假设你对设备说：

> 明天下午三点开会，并提前十分钟提醒。

按目标架构，它不会从麦克风直接写 SQLite。完整路线是：

```text
I2S 麦克风采集 PCM
  -> VoiceSession / Provider / 云端 ASR
  -> 得到文字和工具调用意图
  -> MCP Tool Gateway / Application 用例
  -> Schedule 领域：生成“明天 15:00 开会”的日程
  -> Timing 领域：生成“明天 14:50 触发”的提醒任务
  -> ScheduleStorePort / TimingTaskStorePort
  -> 领域 SQLite Adapter：把业务动作翻译成命名语句
  -> 一个 StorageWriteRequest
  -> StorageTransactionPort::Commit()
  -> SQLite 事务
  -> SQLite VFS
  -> FATFS
  -> Wear Levelling
  -> Flash
```

这里至少产生两条互相关联的业务事实：日程本身和提醒任务。如果设备在第一条写完后复位，而第二条没写，用户会看到一个永远不提醒的日程。因此应用层要把相关写入交给同一笔事务，而不是分两次“尽量写完”。

这条端到端路线目前还没有全部接通。MCP、Schedule、Timing 和存储协议各自已有代码骨架或业务实现，但正式领域 SQLite Adapter 尚未接进默认 Runtime。上图描述的是应当怎样连接，不是声称当前固件已经完成语音建日程闭环。

### 5.3 四层各自知道什么

| 层 | 它知道什么 | 它不应该知道什么 |
| --- | --- | --- |
| Schedule/Timing Store Port | 日程、提醒、到期任务等业务语义 | SQL、表名、FATFS、Flash 扇区 |
| 领域 SQLite Adapter | 如何把 `CreateSchedule` 等业务操作映射成 `calendar.create`、`timing.claim_due` 等命名语句，并把查询行还原成领域对象 | I2S 音频帧、WebSocket、GPIO |
| `StorageTransactionPort` | 结构化读写请求、事务提交、幂等上下文、健康信息 | “开会”是什么意思、提醒为何提前十分钟 |
| SQLite/VFS/FATFS/WL | SQL 执行、日志、文件和 Flash 持久化 | 用户意图和语音会话 |

这和语音模块的 Port/Adapter 思路一致。`ScheduleStorePort` 像 Java `interface`，领域 SQLite Adapter 是实现类；以后也可以有内存 Adapter 供测试使用。业务服务依赖 Port，不应拿着 `sqlite3*` 到处执行 SQL。

### 5.4 为什么统一协议只接受“命名语句”

[`storage_protocol.h`](../../components/voicelife_storage_sqlite/include/voicelife/storage_sqlite/storage_protocol.h#L13) 定义的不是通用 SQL 执行器，而是一个受限协议：

- `StorageValue`：允许传递整数、浮点、布尔、字符串、二进制和空值；
- `StorageStatement`：稳定名字加结构化参数，例如 `calendar.create`；
- `StorageRequestContext`：请求 ID 和业务截止时间；
- `StorageReadRequest` / `StorageReadResult`：一次命名读取、结构化结果和快照 revision；
- `StorageWriteRequest`：同一事务内要执行的一组命名语句；
- `StorageWriteReceipt`：事务 ID、影响行数、耗时、是否提交、是否为重放；
- `StorageHealth`：schema revision、剩余空间、提交次数和最慢提交耗时。

主机契约测试里的最小例子是：

```cpp
StorageWriteRequest write{
    .context = {.request_id = "req-42", .deadline_ms = 3000},
    .statements = {{.name = "calendar.create",
                    .arguments = {std::int64_t{42}, true}}},
};
```

`calendar.create` 不是 SQL，它是一个稳定的操作名。生产 Adapter 内部才知道它对应哪些预编译 SQL。这样做有三个好处：业务模块不能越界修改任意表；SQLite 的表结构和句柄不会扩散到所有组件；以后修改表结构时，可以在 Adapter 内迁移，而不用改每个调用者。

校验代码在 [`storage_protocol.cc`](../../components/voicelife_storage_sqlite/src/storage_protocol.cc#L23)。空 `request_id`、零 deadline、空写入批次和 `INSERT schedule` 这类原始 SQL 会在进入数据库前被拒绝。当前实现到这里主要还是协议/TDD 骨架：它验证请求形状，但还没有执行真实生产事务。

### 5.5 原子性、幂等和 Outbox 分别解决什么事故

一次“创建日程并设置提醒”的设计事务会覆盖四类数据：

```text
Schedule       日程事实
TimingTask     何时提醒
request_dedup  这个 request_id 是否已经处理过，以及原结果是什么
Outbox         提交后还要可靠发送给其他模块或云端的事件
```

**原子性**解决“只写了一半”。四类数据放在同一个 `StorageWriteRequest` 中，最终实现必须在一个 SQLite 事务里提交。任一语句失败，整笔回滚。

**幂等**解决“网络超时后重试造成重复日程”。每次业务请求带 `request_id`。目标语义是：

- 同一个 `request_id`、相同内容再次到达，返回第一次的结果，并把回执标为 `replayed=true`；
- 同一个 `request_id`、不同内容再次到达，返回冲突，不能悄悄覆盖第一次结果；
- 新 `request_id` 才创建新业务事实。

协议里的 `request_id` 和 `StorageWriteReceipt::replayed` 已经为此留出边界；完整的内容摘要、结果回读和冲突处理仍属于正式 Adapter 的实现工作。

**Outbox**解决“数据库写成功了，但通知没发出去”。先把待发送事件与业务数据放在同一事务里；事务成功后，后台任务再发送 Outbox。发送失败可以重试，不会丢掉“有一条事件待发”这个事实。它不是音频播放队列，也不应在 I2S 任务中同步发送。

### 5.6 为什么坚持单连接、单写者

Flash 写入慢，而且 SQLite 同一时刻只能有受控的写事务。让多个 FreeRTOS 任务各拿一个连接同时写，会带来锁竞争、不可预测延迟、额外内存占用和更难复现的复位故障。

设计选择是：业务任务提交结构化请求，存储 worker 用一个连接串行写入。队列必须有界；超时、队列满和容量不足要明确返回错误，不能无限堆积。读取也通过协议返回结构化行，不把连接或 `sqlite3*` 借给调用方。

这也解释了为什么数据库不在语音数据面：音频任务每 10/20/60 ms 就要处理下一段数据，而实板 SQLite 提交是秒级。数据库 worker 可以慢，麦克风 DMA 不能等它。

### 5.7 #104 的实板实验到底证明了什么

[`board-storage-validation.md`](./board-storage-validation.md#L7) 固定的通过基线是：

```text
SQLite               3.53.4
文件系统             FATFS + Wear Levelling
扇区                  4 KiB
journal_mode          DELETE
synchronous           EXTRA
powersafe overwrite   0
数据库所有者          单连接 / 单写者
```

四轮独立实验通过了显式回滚、每轮 40 次四表原子提交、表/索引一致性、`PRAGMA quick_check`、关闭重开、幂等唯一约束，以及两个外部 EN 复位故障点。四轮平均提交耗时的中位数约 `1.16 秒`，最慢一次约 `1.52 秒`。

由此能得出两个结论。第一，当前板上只允许继续走 `SQLite + FATFS/Wear Levelling` 基线；测试过的 LittleFS 组合在显式回滚后出现“表里有行、索引里没记录”，已被否决。第二，数据库提交绝不能运行在 I2S、AFE 或 Provider 实时任务里，必须交给独立的单写者 worker。

但这不是完整生产认证。EN 控制线复位比 `esp_restart()` 更接近故障恢复，仍不等于真正切断电源，也不能替代棕断测试。

### 5.8 为什么探针要备份 OTA 槽，又为什么这不属于数据库业务代码

板上没有专门空闲的大测试分区，所以验证脚本临时使用人工确认的非活动 OTA 槽运行探针。安全顺序是：

```text
从芯片读取分区表
  -> 备份业务数据分区、目标 OTA 槽、otadata
  -> 校验备份摘要
  -> 只写确认过的非活动 OTA 槽
  -> 串口监控 phase 和 EN 复位点
  -> 回读验证
  -> 恢复业务数据
  -> 恢复原 OTA 槽
  -> 最后恢复 otadata
```

这套流程保护的是实板和原固件，不是产品运行时的数据访问方式。产品固件不会为了保存一条日程去改 OTA 槽。相关工具入口是 [`sqlite_board_probe.py`](../../scripts/sqlite_board_probe.py#L157)，完整操作要求见 [`board-storage-validation.md`](./board-storage-validation.md#L61)。

### 5.9 数据库现在完成了什么，还缺什么

| 状态 | 内容 |
| --- | --- |
| 已完成 | 统一存储协议类型、请求形状校验、主机契约测试 |
| 已完成 | SQLite/FATFS/WL 实板资格基线、故障探针、备份和恢复流程 |
| 设计已明确 | 领域 Store Port -> 领域 SQLite Adapter -> `StorageTransactionPort` -> SQLite 的边界 |
| 尚未完成 | 正式 Schedule/Timing SQLite Adapter 和命名语句实现 |
| 尚未完成 | schema 创建与迁移、损坏数据库受限启动、容量耗尽与恢复 |
| 尚未完成 | 可控断电/棕断、长期写放大和磨损寿命测试 |
| 尚未完成 | 默认 Runtime 装配，以及音频和数据库并行时的队列、延迟、内存验证 |

因此，读到 `StorageTransactionPort::Commit()` 时，正确说法是“仓库定义了未来生产 Adapter 必须遵守的提交契约”；不能说“现在日程已经通过它写进板上 SQLite”。这正是空骨架的价值：先把边界定死，再补实现，同时不伪造完成度。

## 6. 你这周到底交付了什么

用人话重述：

1. **#92：先把房子的承重墙画出来。** 组件、依赖方向、公共接口、Runtime 入口、主机测试和 CI 门禁先固定，避免后面每加一个功能就重新搭主干。
2. **#104：先证明存储方案不会把板子和数据一起弄坏。** 你做了故障探针、备份/回读/恢复工具和 SQLite 控制面协议，但没有假装业务 Adapter 已完成。
3. **#106：把“语音服务”从会话核心里拔出来。** `VoiceSession` 只懂稳定语义；Linx 的字段、编码、连接细节留给 Provider 和 Codec。
4. **#108：把 WSS 接到 ESP32-S3，但把回调和会话隔开。** 网络事件先进 FreeRTOS 队列，再由工作任务分发；消息分片先重组；连接关闭、重连、队列溢出都有明确状态。
5. **#110：不再猜板型。** 用 Profile 表达 GPIO、I2S、Codec 和 DMA，用探针在串口上拿证据。
6. **#112：承认实际板子是纯 I2S，而不是 Lichuang Codec 板。** 你新建了独立 Profile，并用削波数据修正了 PCM 对齐。
7. **#114：把“探针能读写”推进到“VoiceSession 有可用的 Audio Port”。** 采集、组帧、发送、播放分任务和有界队列，打断时用 generation 丢弃旧帧。
8. **#117：删掉过时的历史说明。** 这不是代码功能，但让 README 不再把阶段性重建背景当成当前使用说明。
9. **#151：规定哪些证据进仓库，哪些留在 Issue/Review。** 语音研究原文留在 Issue #150，仓库只保留当前代码需要的架构、硬件验证和稳定结论。

## 7. 当前做到哪一步，哪些还没做

### 已经有证据的部分

- 组件边界、Port 契约、主机契约测试和 ESP-IDF 构建。
- SQLite/FATFS/Wear Levelling 的实板资格基线和可恢复探针流程。
- Linx JSON 的 hello/listen/stt/tts/error 协议解析。
- ESP32-S3 WSS/TLS Transport 的连接、事件队列、分片重组和音频二进制发送。
- `voicelife-pcb` 纯 I2S PCM 的 I2S 生命周期、数字采集、有限回放和资源统计。

### 仍然没有被这批 PR 证明的部分

- 默认 Runtime 已按 Profile 自动装配真实 Linx Provider。
- ES8311/ES7210/PCA9557 的 Codec 控制面和真实模拟录放。
- Opus、AFE、AEC、WakeNet、真实云端 WSS、ASR/TTS 物理闭环。
- SQLite 正式业务 Adapter、schema 迁移、容量耗尽和长期磨损。
- 人在设备前完成“唤醒 -> 讲话 -> 工具调用 -> 播报 -> 打断 -> 断网恢复”。

README 也明确写了语音和 Linx 仍在开发中，WSS 有构建和主机契约，但真实云端与声学闭环未完成（[`README.md`](../../README.md#L121)）。

## 8. 术语小抄

| 术语 | 白话 | 在本仓库里对应什么 |
| --- | --- | --- |
| WSS | 加 TLS 加密的 WebSocket 长连接 | `voicelife_linx_esp` |
| TLS | 给网络连接加密并验证服务器身份的协议 | `esp_crt_bundle_attach` |
| WebSocket frame | WebSocket 传输中的一小段消息 | `WebSocketFragmentAssembler` |
| PCM | 未压缩的数字音频采样 | `AudioFrame` 的 `kPcmS16Le` |
| I2S | 芯片和音频器件之间的数字音频总线 | ESP32-S3 RX/TX 通道 |
| DMA | 外设和内存之间自动搬运数据 | I2S `dma_desc_num` / `dma_frame_num` |
| Codec | 模拟音频和数字音频之间的编解码芯片 | ES8311、ES7210 |
| ASR | Automatic Speech Recognition，语音转文字 | Linx `stt` 事件 |
| TTS | Text To Speech，文字转语音 | Linx `tts` 事件和下行音频 |
| Provider | 一套语音服务的完整适配器 | `LinxSpeechProviderAdapter` |
| Port | 核心定义的接口插座 | `AudioInputPort`、`VoiceTransportPort` |
| Adapter | 把具体硬件/协议接到 Port | `EspWebSocketTransport`、`Esp32s3PcmAudioPorts` |
| generation | 当前会话的版本号 | 打断后拒绝旧音频帧/事件 |
| bounded queue | 有固定容量的队列 | 防止网络阻塞麦克风或无限吃内存 |
| Profile | 一套板型和能力配置 | `esp32s3-voicelife-pcb-pcm` |
| transaction | 一组数据库操作要么全成功，要么全回滚 | `StorageTransactionPort::Commit` |
| 幂等 | 同一个请求重试，不会重复写出另一份结果 | `request_id` 和 replayed |
| FATFS | 面向嵌入式 Flash 的文件系统 | SQLite 的文件承载层 |
| Wear Levelling | 把写入分散到不同 Flash 扇区，延长寿命 | 当前 SQLite 实板基线 |
| OTA slot | 固件的 A/B 分区 | 探针只写非活动槽，结束后恢复 |

## 9. 推荐的阅读顺序

不要从 670 行的硬件源文件开始。按下面顺序，每次只回答一个问题：

1. [`main/main.cc`](../../main/main.cc#L10)：设备从哪儿启动？
2. [`components/voicelife_runtime/src/runtime.cc`](../../components/voicelife_runtime/src/runtime.cc#L152)：谁负责组装对象？默认为什么还是 scaffold？
3. [`components/voicelife_voice/include/voicelife/voice/voice_ports.h`](../../components/voicelife_voice/include/voicelife/voice/voice_ports.h#L25)：核心需要哪些接口？
4. [`components/voicelife_voice/src/voice_session.cc`](../../components/voicelife_voice/src/voice_session.cc#L51)：一轮语音会话如何开始、收音、播放、打断？
5. [`components/voicelife_linx/src/linx_json_codec.cc`](../../components/voicelife_linx/src/linx_json_codec.cc#L104)：控制消息长什么样？
6. [`components/voicelife_linx_esp/src/esp_websocket_transport.cc`](../../components/voicelife_linx_esp/src/esp_websocket_transport.cc#L54)：WSS 连接、队列和分片如何工作？
7. [`components/voicelife_audio_esp/src/audio_board_profile.cc`](../../components/voicelife_audio_esp/src/audio_board_profile.cc#L165)：实际板子的 GPIO 和 PCM 对齐是什么？
8. [`components/voicelife_audio_esp/src/esp32s3_audio_probe.cc`](../../components/voicelife_audio_esp/src/esp32s3_audio_probe.cc#L104)：串口上的 PASS 到底证明了什么？
9. [`components/voicelife_storage_sqlite/include/voicelife/storage_sqlite/storage_protocol.h`](../../components/voicelife_storage_sqlite/include/voicelife/storage_sqlite/storage_protocol.h#L13)：数据库边界为什么不能接进实时音频任务？

读完第 4 步，你应该能自己画出“麦克风帧从哪里来、经过谁、什么时候会被丢弃”。读完第 9 步，你应该能解释“为什么 SQLite 提交不能放进 AudioInputTask”。这两句话能说清楚，说明主干已经入门。
