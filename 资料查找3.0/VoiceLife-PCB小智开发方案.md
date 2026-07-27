# VoiceLife PCB 版小智开发方案

> 日期：2026-07-27
> 选型：PCB 版小智（基于开源 xiaozhi-esp32，ESP32-S3-N16R8 模组）
> 架构：全本地——数据增删改查在开发板上完成，不另起服务端

---

## 一、架构总览

```
用户语音 → 板载 I2S 麦克风 → ESP-SR 唤醒词检测
    ↓ "小鹏小鹏"
ESP32 Wi-Fi → 云端灵矽（ASR 语音识别 + LLM 理解 + Function Calling + TTS 语音合成）
    ↓ Function Calling 结果返回
ESP32 本地 L3 领域服务（日历/提醒/撤销/变更/备忘录）
    ↓
SQLite 本地数据库（日程/提醒/备忘录/撤销快照）
    ↓
本地定时器触发 → I2S 功放 + 喇叭播报提醒
    ↓
Wi-Fi → 飞书 Webhook 推送 IM 回执
```

> 云端负责语音处理（ASR/TTS）+ LLM 推理。业务逻辑、数据存储、定时任务在 ESP32 本地闭环。音频数据经加密传输到灵矽云端，处理后返回。

### 与 B/S 架构的关键区别

| 传统方案 | 本方案 |
| --- | --- |
| 开发板 = 瘦客户端，所有逻辑在服务端 | 开发板 = 全栈主机，本地处理一切 |
| 数据库在服务器上 | SQLite 在 ESP32 Flash/TF 卡上 |
| 定时任务靠服务端 cron | ESP32 本地 FreeRTOS 定时器 + NTP 对时 |
| 断网不可用 | 断网时不可查询（无本地识别入口）（仅 LLM 功能不可用） |
| 需要维护服务器 | 插电即用 |

---

## 二、硬件配置

### 核心模组

| 参数 | 值 |
| --- | --- |
| 主控 | ESP32-S3-N16R8（双核 Xtensa LX7 @ 240MHz） |
| Flash | 16MB Octal SPI |
| PSRAM | 8MB Octal SPI |
| Wi-Fi | 2.4GHz 802.11 b/g/n |
| BLE | Bluetooth 5.0 LE |
| AI 加速 | 向量指令扩展（SIMD） |

### 板载外设

| 外设 | 型号/规格 | 用途 |
| --- | --- | --- |
| 麦克风 | I2S MEMS 数字麦克风（INMP441 或同类） | 语音输入 |
| 功放 + 喇叭 | MAX98357 I2S 功放 + 3W/4Ω 喇叭 | 语音播报 |
| LED | WS2812 × 1-3 | 状态指示（唤醒/思考/说话/空闲） |
| 按键 | BOOT + RST + 功能键 | 烧录模式 + 手动唤醒 |
| USB | USB-C | 烧录 + 供电 + 串口日志 |
| TF 卡槽 | SPI 接口（PCB 版需确认是否预留） | SQLite 外置存储（可选） |

### 不自带、需外接的（与 HiWonder/BOX-3 的区别）

| 缺失项 | 影响 | Workaround |
| --- | --- | --- |
| 显示屏 | 无法板载显示 IM 回执 | IM 回执通过飞书/微信手机端展示 |
| 锂电池管理 | 无电池供电 | USB 供电或外接充电宝 |
| 摄像头 | 无多模态扩展 | 本期不做多模态 |
| 硬件 RTC | 断网时间漂移 | NTP 联网校准（断网期间有 ±数秒偏差，可接受） |

---

## 三、软件架构

### 基于开源 xiaozhi-esp32 改造

```
原版小智：
  ASR → 云端 LLM（聊天）→ TTS → 喇叭
  MCP 控制：音量/LED/电机

VoiceLife 改造：
  ASR → 云端 LLM（调用 Function Calling）→ 本地 L3 服务 → SQLite → TTS → 喇叭
  MCP/FuncCall 控制：日历/提醒/撤销/变更/备忘录
```

### 改造点

| 文件 | 改造内容 | 工作量 |
| --- | --- | --- |
| `main/application.cc` | 替换 System Prompt 为日程助手；修改初始化流程 | 0.5 天 |
| `main/mcp_server.cc` | 替换 MCP tool schema 为五大领域服务 | 1 天 |
| `main/audio/wake_words/custom_wake_word.cc` | "小鹏小鹏"替换默认唤醒词（需确认 ESP-SR 模型支持自定义唤醒词训练） | 0.5-1 天 |
| 新增 `components/voice_life/` | L3 五大服务实现（日历/提醒/撤销/变更/备忘录） | 3-4 天 |
| 新增 `components/sqlite_db/` | SQLite 数据库集成 + 建表 + CRUD 封装 | 1-2 天 |
| 新增 `components/im_push/` | 飞书 Webhook HTTP 客户端 | 1 天 |
| 新增 `components/local_timer/` | NTP + FreeRTOS 定时器提醒触发 | 1 天 |
| 新增 `components/board_voice_life/` | 板级配置（替代 HiWonderExploit_S3 或新建） | 0.5 天 |

### 数据流

```
用户说"明天下午7点开会"
  → ESP-SR 唤醒词检测 → 音频流上传灵矽
  → 灵矽返回 Function Call: create_event("开会", "2026-07-28 19:00")
  → voice_life/calendar_service.cc 处理
  → sqlite_db 写入 events 表
  → local_timer 注册 2026-07-28 18:50 预告定时器
  → TTS 播报"妥了，明天下午7点提醒你开会"
  → im_push 发送飞书回执
```

---

## 四、开发环境

### macOS 上搭建

```bash
# 1. 安装 ESP-IDF
brew install esp-idf
# 选择 v5.4.3 或 v5.5.x（匹配 PCB 源码版本）

# 2. 安装烧录工具
pip install esptool

# 3. 验证环境
idf.py --version

# 4. 解压源码
unzip 小智源码/xiaozhi-esp32-main.zip -d voice_life_firmware
cd voice_life_firmware

# 5. 设置目标芯片 + 首次编译验证
idf.py set-target esp32s3
idf.py menuconfig   # 检查 Flash 16MB / PSRAM 8MB
idf.py build        # 能编过 → 环境 OK

# 6. 烧录
idf.py -p /dev/cu.usbmodem* flash monitor
```

> CPU 要求：macOS Apple Silicon（M1/M2/M3）原生支持交叉编译，无需 Rosetta。Intel Mac 也同样支持。

---

## 五、关键问题与解决方案

### 5.1 SQLite 怎么集成到 ESP32？

通过 ESP-IDF 组件管理器添加 SQLite。先在 [ESP Component Registry](https://components.espressif.com) 确认可用的 SQLite 组件名称和版本号，然后添加到 `main/idf_component.yml`：

```yaml
dependencies:
  <sqlite-component-name>: "<version>"
```

> 组件名和版本需在集成时从官方 Registry 确认。还需验证 VFS 挂载、WAL 模式、线程安全模式和断电恢复行为——不能假设默认配置适用于本场景。

### 5.2 能存多少条数据？

| 场景 | 数据量 | SQLite 文件大小 |
| --- | --- | --- |
| 100 条日程 + 50 条记录 | 日常使用 | ~10KB |
| 1000 条日程 + 500 条记录 | 重度使用 | ~60KB |
| 10000 条日程 + 撤销快照 | 全年数据 | ~500KB |
| **16MB Flash 可用空间** | 扣除系统+OTA | **~待实测** |
| **理论上限** | | **~待实测（依赖分区表、固件体积、索引开销）** |

> 结论：个人使用待实测后确认

### 5.3 SQLite 存 Flash 还是 TF 卡？

| 选项 | 优点 | 缺点 |
| --- | --- | --- |
| **Flash（SPIFFS/LittleFS）** | 无需额外硬件，读写快 | 擦写寿命 ~10 万次，写坏了整个板子报废 |
| **TF 卡（推荐）** | 可插拔更换，数据可拔卡在电脑查看 | 需确认 PCB 版是否有 TF 卡槽 |

> 如果 PCB 版无 TF 卡槽：Flash 日常使用可支撑数年。需配置 ESP-IDF 的 wear leveling 和 SPIFFS/LittleFS 磨损均衡，避免 SQLite 写放大集中在固定扇区。

### 5.4 断网了怎么办？

| 功能 | 断网表现 |
| --- | --- |
| 语音创建/查询 | 不可用（LLM 需要网络） |
| 已存数据查询 | 不可用（无屏无本地语音识别，无法输入查询指令） |
| 提醒触发 | **可用**（本地定时器 + NTP 最后一次对时） |
| 时间准确性 | 漂移 < 1 秒/小时（无 RTC 硬件），断网几小时内影响可忽略 |
| IM 回执 | 不可用（需网络），网络恢复后补发 |

### 5.5 Wi-Fi 配网怎么处理？

小智源码自带两种配网方案，直接复用：
- **热点配网**：ESP32 开机自建热点 → 手机连接 → 浏览器输入 Wi-Fi 密码
- **BluFi**：手机蓝牙传 Wi-Fi 密码

首次使用配一次即可，后续自动重连。

### 5.6 多并发怎么处理？（如同时有 3 条提醒触发）

ESP32 双核 FreeRTOS：一个核跑音频管道（音频采集 + Opus 编码），另一个核跑业务逻辑（定时器 + SQLite + Webhook）。同时触发多条提醒时，定时器队列依次触发，语音按 P2 原型规则逐条播报，不并发抢占喇叭。

### 5.7 OTA 升级怎么做？

小智源码自带 OTA（`main/ota.cc`），可复用。通过 Wi-Fi 下载新固件 → 校验 → 写入 OTA 分区 → 重启。自动回滚依赖分区表配置和 Bootloader 的 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，需在 menuconfig 中显式开启。

### 5.8 功耗和供电？

ESP32-S3 全速运行约 300mA @ 5V。USB-C 供电（手机充电器即可）。如果用充电宝：10000mAh 充电宝可持续运行约 30 小时。后续如需电池供电，PCB 需外加 TP4056 充电模块 + 3.7V 锂电池。

---

## 六、开发路线图

| 阶段 | 内容 | 时间 |
| --- | --- | --- |
| **第 1 步** | 搭建 ESP-IDF 环境 + 编译 PCB 源码通过 | 半天 |
| **第 2 步** | 替换唤醒词 "小鹏小鹏" + 修改 System Prompt | 半天 |
| **第 3 步** | 集成 SQLite + 建表 + 基础 CRUD 测试 | 1-2 天 |
| **第 4 步** | 实现 L3 五大服务 + Function Calling schema | 3-4 天 |
| **第 5 步** | 实现本地定时提醒触发 + NTP | 1 天 |
| **第 6 步** | 实现 IM Webhook 推送 | 1 天 |
| **第 7 步** | P1+P2+P3 端到端测试 | 1-2 天 |
| **合计** | | **约 8-11 天** |

---

## 七、风险清单

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| PCB 版无 TF 卡槽，SQLite 只能存 Flash | 中 | 低——Flash 寿命够用数年 | 提前确认 PCB 设计 |
| 灵矽平台 Function Calling 格式与小智现有协议不兼容 | 中 | 高——需修改 `protocols/websocket_protocol.cc` | 提前对接灵矽 API 文档 |
| 唤醒词 "小鹏小鹏" 识别率低 | 低 | 中——ESP-SR 支持自定义训练 | 录制样本重新训练或降级为按键唤醒 |
| IM Webhook 推送在弱网下丢消息 | 中 | 低——用户仍可语音查询本地数据 | HTTP 重试 + 本地消息队列缓存 |
| macOS ESP-IDF 编译大工程耗时 | 低 | 低——仅影响开发体验 | 首次全量编译 ~5 分钟，增量 ~30 秒 |
