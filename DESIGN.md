---
version: "beta"
name: "GitDay 黑白灰工作台"
description: "用于 Google Stitch 生成 GitDay 中文产品原型的设计系统与页面契约。页面固定为 10 个，固定组件位置不变，功能来源只允许来自 PR #30 自动导入任务、PR #28 任务优先级排序与展示、PR #29 任务自动重排，并参考 arch.md 的模块数据流。"
platform: "macOS desktop app prototype"
screen_count: 10
language: "zh-CN"
colors:
  canvas: "#F7F7F8"
  surface: "#FFFFFF"
  surface-muted: "#F1F1F2"
  surface-raised: "#FAFAFA"
  text: "#18181B"
  text-secondary: "#52525B"
  text-muted: "#8A8A93"
  border: "#D4D4D8"
  border-soft: "#E4E4E7"
  ink: "#27272A"
  ink-hover: "#3F3F46"
  ink-soft: "#EDEDEF"
  receipt: "#FAFAFA"
  danger-gray: "#3F3F46"
  on-ink: "#FFFFFF"
typography:
  page-title:
    fontFamily: "SF Pro Display, PingFang SC, Microsoft YaHei, -apple-system, BlinkMacSystemFont, sans-serif"
    fontSize: "24px"
    fontWeight: 700
    lineHeight: "30px"
    letterSpacing: "0px"
  section-title:
    fontFamily: "SF Pro Text, PingFang SC, Microsoft YaHei, -apple-system, BlinkMacSystemFont, sans-serif"
    fontSize: "16px"
    fontWeight: 600
    lineHeight: "24px"
    letterSpacing: "0px"
  body:
    fontFamily: "SF Pro Text, PingFang SC, Microsoft YaHei, -apple-system, BlinkMacSystemFont, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: "22px"
    letterSpacing: "0px"
  label:
    fontFamily: "SF Pro Text, PingFang SC, Microsoft YaHei, -apple-system, BlinkMacSystemFont, sans-serif"
    fontSize: "12px"
    fontWeight: 500
    lineHeight: "18px"
    letterSpacing: "0px"
  mono:
    fontFamily: "SF Mono, JetBrains Mono, ui-monospace, Menlo, Consolas, monospace"
    fontSize: "12px"
    fontWeight: 500
    lineHeight: "18px"
    letterSpacing: "0px"
rounded:
  xs: "4px"
  sm: "6px"
  md: "8px"
spacing:
  xs: "4px"
  sm: "8px"
  md: "16px"
  lg: "24px"
  xl: "32px"
components:
  app-shell:
    backgroundColor: "{colors.canvas}"
    textColor: "{colors.text}"
  top-toolbar:
    backgroundColor: "{colors.surface}"
    height: "56px"
    borderBottom: "1px solid {colors.border-soft}"
  sidebar:
    backgroundColor: "{colors.surface-muted}"
    width: "216px"
    borderRight: "1px solid {colors.border-soft}"
  main-header:
    backgroundColor: "{colors.canvas}"
    height: "72px"
  right-inspector:
    backgroundColor: "{colors.surface}"
    width: "340px"
    borderLeft: "1px solid {colors.border-soft}"
  task-row:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text}"
    height: "56px"
    rounded: "{rounded.md}"
  task-row-selected:
    backgroundColor: "{colors.ink-soft}"
    textColor: "{colors.text}"
    height: "60px"
    rounded: "{rounded.md}"
  button-primary:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.on-ink}"
    rounded: "{rounded.md}"
    height: "36px"
    padding: "0 14px"
  button-secondary:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text}"
    border: "1px solid {colors.border}"
    rounded: "{rounded.md}"
    height: "36px"
    padding: "0 14px"
  receipt-panel:
    backgroundColor: "{colors.receipt}"
    textColor: "{colors.text}"
    border: "1px solid {colors.border}"
    rounded: "{rounded.md}"
---

# GitDay 黑白灰工作台 DESIGN.md

这份文档把 GitDay 原型重新收敛为 **10 个中文 macOS 桌面页面**。Stitch 生成时必须固定顶部工具栏、左侧导航、主内容标题区、模块状态轨道、右侧详情栏和变化回执槽，不允许把状态随意生成成新的侧边栏 Tab。

下一步：把本文件上传 Stitch 生成原型。验收时先核对页面数量、固定组件和中文文案，再看视觉细节。

## 0. 来源确认

最近仓库工作集中在产品原型与三个 Proposal 收敛，不是在写正式业务代码：

| 来源 | 对原型的有效输入 |
| --- | --- |
| 当前分支 `zhaoxingpeng/issue-31-prototype-design` | 2026-07-14 新增产品原型文档和移动端 Live Demo 原型。 |
| PR #30 自动导入任务 | GitHub 授权、选择单个 Repository、单向导入与我有关的 Issue / PR / Review 请求、初始化缺失字段。 |
| PR #28 任务优先级排序与展示 | P0/P1/P2、硬截止、预计投入、无 DDL 老化上浮、手动顺序保留、排序理由可解释。 |
| PR #29 任务自动重排 | 三个主状态、待评审不占容量、阻塞和待估算不进入今日计划、容量不足回到规划池、硬截止冲突由用户处理、Done 与 Reopen 回流。 |
| `资料查找/arch.md` | 数据流固定为 GitHub Connection -> Task Pool -> Prioritization -> Daily Planning，由 Workspace Coordinator 串联。UI 只展示结果，不绕过模块直接改数据。 |

本文件不得引入上述来源之外的新功能。

## 1. 固定输出契约

Stitch 必须生成 exactly **10 screens**，全部为中文 macOS 桌面端应用页面。

不得生成：

- 11 个及以上页面；
- 手机壳、iPhone 页面、平板页面；
- 营销首页、组件库展示页、纯 design system 页；
- 普通 ToDo、番茄钟、日历、甘特图、团队看板；
- 随机侧边栏 Tab，例如把 `待评审`、`阻塞`、`待估算`、`截止冲突` 单独生成成主导航；
- 英文大面积界面；
- 彩色主题、GitHub 绿主色、蓝紫渐变、霓虹、玻璃拟态。

## 2. 页面清单

页面固定为下列 10 个。页面编号、中文标题和目的不得改名。

| 编号 | 页面名 | 来源 | 目的 |
| --- | --- | --- | --- |
| S01 | GitHub 授权 | PR #30 | 说明只读授权范围和不写回 GitHub 的边界。 |
| S02 | 选择仓库与 Milestone | PR #30 | 选择一个仓库和当前 Milestone，确认 MVP 不做多仓库。 |
| S03 | 导入与初始化 | PR #30 | 展示 Issue / PR / Review 请求如何进入个人任务池，以及哪些字段缺失。 |
| S04 | 我的工作 | PR #30 + arch.md | 展示 Task Pool 归一后的个人行动项，不等于今日计划。 |
| S05 | 工作台总览 | 三个 Proposal | 主页面：今日焦点、任务池摘要、最新计划变化和右侧证据。 |
| S06 | 今日焦点 | PR #28 + PR #29 | 展示今天 1 至 3 个可执行任务、容量和排序依据。 |
| S07 | 排序理由 | PR #28 | 展示一项任务为什么排在当前位置。 |
| S08 | 评审反馈 | PR #29 | 展示 PR feedback 证据，以及哪些反馈会让任务回到需处理。 |
| S09 | 计划调整 | PR #29 | 展示变化回执、规划池、待补估算、阻塞和硬截止冲突。 |
| S10 | 已结束与重开 | PR #29 | 展示 merged / closed / reopened 后任务如何退出或回流。 |

## 3. 固定组件契约

所有页面必须使用同一套应用外壳。S01 和 S02 也不能做成营销页或孤立表单，它们只是同一桌面应用中的首次配置状态。

### 3.1 全局布局

默认画布：`1440 x 960`。

```text
macOS 标题栏 44px
┌──────────────────────────────────────────────────────────────┐
│ 顶部工具栏 56px：产品 / 仓库 / Milestone / 同步 / 搜索 / 用户 │
├──────────────┬────────────────────────────────┬──────────────┤
│ 左侧导航     │ 主内容区                        │ 右侧详情栏   │
│ 216px 固定   │ 标题区 + 状态轨道 + 页面主体     │ 340px 固定   │
└──────────────┴────────────────────────────────┴──────────────┘
```

固定尺寸：

| 组件 | 尺寸 | 规则 |
| --- | --- | --- |
| macOS 标题栏 | 44px 高 | 每页都有 traffic-light controls。 |
| 顶部工具栏 | 56px 高 | 每页都有，内容位置不变。 |
| 左侧导航 | 216px 宽 | 每页都有，S01-S03 可禁用部分入口，但不能隐藏。 |
| 主内容标题区 | 72px 高 | 每页都有标题、副标题和主动作槽。 |
| 模块状态轨道 | 56px 高 | 每页都有，当前节点高亮。 |
| 主内容区 | 自适应 | 页面主体只在这里变化。 |
| 右侧详情栏 | 340px 宽 | 每页都有，分组顺序固定。 |
| 变化回执槽 | 右侧详情栏底部 | 每页都有；没有变化时显示 `暂无计划变化`。 |

### 3.2 顶部工具栏固定内容

从左到右固定为：

1. 产品标识：`GitDay`。
2. 当前连接：`未连接` / `已连接 ZhaoXingPeng`。
3. 仓库：未选择时显示 `未选择仓库`，选择后显示 `1024XEngineer / XE6-15`。
4. Milestone：未选择时显示 `未选择 Milestone`，选择后显示当前 Milestone。
5. 同步状态：`等待授权` / `刚刚同步` / `同步失败，显示上次结果`。
6. 搜索入口：`搜索 Issue、PR`。
7. 命令入口：`命令`，可标注 `Cmd K`。
8. 用户入口：头像或中文用户名。

工具栏不能因为页面不同而增删字段，只能改变字段值和禁用状态。

### 3.3 左侧导航固定内容

左侧导航只允许出现这 6 个入口，顺序固定：

1. `工作台`
2. `我的工作`
3. `今日焦点`
4. `评审反馈`
5. `计划调整`
6. `已结束`

底部固定显示：`只读 GitHub 模式`。

禁止把下列内容生成为侧边栏主导航：`待评审`、`等待评审`、`阻塞任务`、`待估算`、`截止冲突`、`规划池`、`Timeline`、`Dashboard`、`Inbox`、`Settings`。

这些内容只能作为页面内分区、任务状态、状态原因或右侧详情。

### 3.4 主内容标题区

每页主内容标题区固定包含：

| 位置 | 内容 |
| --- | --- |
| 左上 | 面包屑，例如 `工作台 / 今日焦点`。 |
| 左下 | 中文页面标题。 |
| 标题下方 | 一句中文说明，不超过 24 个汉字。 |
| 右侧 | 一个主动作槽；没有主动作时显示次按钮或留空占位。 |

标题区不能塞功能说明长文。

### 3.5 模块状态轨道

状态轨道表达架构数据流，不是业务 Tab。节点固定为：

```text
GitHub 连接 -> 任务池 -> 排序结果 -> 今日计划
```

节点对应模块：

| 轨道节点 | 模块 | 页面高亮 |
| --- | --- | --- |
| GitHub 连接 | GitHub Connection | S01、S02 |
| 任务池 | Task Pool | S03、S04 |
| 排序结果 | Prioritization | S07 |
| 今日计划 | Daily Planning | S05、S06、S08、S09、S10 |

S08-S10 虽然处理反馈、调整和完成，但仍高亮 `今日计划`，因为它们展示 Daily Planning 的结果或回流。

### 3.6 右侧详情栏固定分组

右侧详情栏每页都存在，分组顺序固定：

1. `GitHub 证据`
2. `当前状态`
3. `排序或计划依据`
4. `计划变化`
5. `可执行动作`

没有内容时保留分组标题，显示 `暂无`，不要删除分组。这样 Stitch 不会在不同页面生成完全不同的右栏结构。

### 3.7 固定组件矩阵

| 页面 | 顶部工具栏 | 左侧导航 | 标题区 | 状态轨道 | 右侧详情栏 | 回执槽 |
| --- | --- | --- | --- | --- | --- | --- |
| S01 GitHub 授权 | 固定，显示未连接 | 固定，入口禁用 | 固定 | 高亮 GitHub 连接 | 固定，显示权限边界 | 显示暂无 |
| S02 选择仓库与 Milestone | 固定，显示已连接 | 固定，入口禁用 | 固定 | 高亮 GitHub 连接 | 固定，显示单仓库边界 | 显示暂无 |
| S03 导入与初始化 | 固定，显示同步中 | 固定，入口禁用 | 固定 | 高亮任务池 | 固定，显示映射规则 | 显示导入摘要 |
| S04 我的工作 | 固定 | 固定，高亮我的工作 | 固定 | 高亮任务池 | 固定，显示选中任务证据 | 显示暂无 |
| S05 工作台总览 | 固定 | 固定，高亮工作台 | 固定 | 高亮今日计划 | 固定，显示选中任务 | 显示最新回执 |
| S06 今日焦点 | 固定 | 固定，高亮今日焦点 | 固定 | 高亮今日计划 | 固定，显示计划依据 | 显示最新回执 |
| S07 排序理由 | 固定 | 固定，高亮今日焦点 | 固定 | 高亮排序结果 | 固定，显示排序理由 | 显示暂无 |
| S08 评审反馈 | 固定 | 固定，高亮评审反馈 | 固定 | 高亮今日计划 | 固定，显示 Review 证据 | 显示触发说明 |
| S09 计划调整 | 固定 | 固定，高亮计划调整 | 固定 | 高亮今日计划 | 固定，显示变化原因 | 显示完整回执 |
| S10 已结束与重开 | 固定 | 固定，高亮已结束 | 固定 | 高亮今日计划 | 固定，显示结束证据 | 显示回流回执 |

## 4. 中文与术语规则

界面文案优先中文。仅保留必要英文技术对象：`GitHub`、`Issue`、`PR`、`Review`、`Milestone`、`OAuth`、`Token`、`Cmd K`。

内部状态值不能直接显示给用户。显示文案固定如下：

| 内部值 | 中文显示 | 用法 |
| --- | --- | --- |
| `on_progress` | 需处理 | 当前需要用户行动。 |
| `pending_review` | 待评审 | 用户已提交或改完，等待 Review 或合并。 |
| `done` | 已结束 | 当前没有待用户处理动作。 |
| `today` | 今日焦点 | 今天 1 至 3 个可执行任务。 |
| `planning_pool` | 规划池 | 今天放不下，但后续仍要处理。 |
| `unestimated` | 待补估算 | 缺预计投入，不能进入今日容量。 |
| `blocked` | 阻塞 | 暂时无法推进，不占今日容量。 |
| `capacity_conflict` | 容量冲突 | 硬截止所需时间超过今日剩余容量。 |

按钮文案固定：

| 场景 | 文案 |
| --- | --- |
| 授权 | `连接 GitHub` |
| 选择仓库 | `使用此仓库` |
| 导入完成 | `进入我的工作` |
| 进入主页面 | `进入工作台` |
| 今日计划 | `查看今日焦点` |
| 排序解释 | `查看排序理由` |
| 反馈触发 | `更新计划` |
| 回执后 | `查看计划调整` |
| 缺估算 | `补充预计投入` |
| 阻塞解除 | `标记为已解除阻塞` |
| 外链 | `打开 GitHub` |

禁止出现空泛英文营销文案，如 `Next-Gen`、`Boost productivity`、`Smart Dashboard`。

## 5. 架构数据流约束

原型表现必须服从 `arch.md` 的模块边界：

```text
GitHub Connection
  -> RepositoryContext
Task Pool
  -> TaskPoolSnapshot
Prioritization
  -> RankingResult
Daily Planning
  -> DailyPlan / PlanChangeSet / CapacityConflict
```

页面不能表现为 UI 直接修改 GitHub、排序模块直接改计划，或 Daily Planning 直接改优先级。

### 5.1 页面与模块映射

| 页面 | 主要展示模块 | 页面可以展示什么 | 页面不能暗示什么 |
| --- | --- | --- | --- |
| S01 | GitHub Connection | 授权状态、只读范围、Token 边界 | Token 被传给其他模块 |
| S02 | GitHub Connection | 当前账号、仓库、Milestone | 多仓库并行同步 |
| S03 | Task Pool | 同步结果、缺字段、失败原因 | 同步失败等于没有任务 |
| S04 | Task Pool | 归一后的 PersonalAction | Task Pool 排序或排计划 |
| S05 | Workspace Coordinator | 串联后的总览结果 | UI 绕过协调器直接调多个模块 |
| S06 | Daily Planning | 今日焦点、容量、计划结果 | 自动写回 GitHub 或日历 |
| S07 | Prioritization | 排序原因代码转成中文解释 | 黑盒 AI 分数 |
| S08 | Task Pool + Daily Planning | Review 证据触发状态变化 | 普通评论自动变成返工 |
| S09 | Daily Planning | PlanChangeSet、CapacityConflict | 静默延期或删除任务 |
| S10 | Task Pool + Daily Planning | Done、Reopen、回流后重排 | closed 未合并显示成已合并 |

### 5.2 必须展示的不变量

1. GitHub Token 不进入 Task Pool、Prioritization、Daily Planning。
2. Task Pool 更新 GitHubFacts，但不覆盖用户的优先级、硬截止、预计投入和手动顺序。
3. Prioritization 只产出排序和原因，不保存今日计划。
4. Daily Planning 只消费排序结果和容量，不改 GitHub，不改优先级。
5. 缺预计投入的任务可以展示排序位置，但不能进入今日容量。
6. 阻塞、待评审、已结束任务不占今日容量。
7. 同样输入、同样规则版本，排序结果稳定。
8. 同步失败时保留上次成功快照和计划。
9. 所有 GitHub 写回能力本期不做。

## 6. 产品边界

目标用户：XEngineer 训练营成员，正在围绕 GitHub Issue、PR、Review 推进自己的任务。

核心问题：GitHub 告诉用户发生了什么，但不直接回答“我今天先处理哪几个，为什么是这个顺序，反馈来了以后计划怎么变”。

本期做：

| 能力 | 来源 |
| --- | --- |
| 绑定 GitHub，选择单个仓库和 Milestone | PR #30 |
| 导入 Assigned Issue、My PR、Review Request | PR #30 |
| 将 GitHub 对象归一成个人行动项 | PR #30 + arch.md |
| 设置或展示 P0/P1/P2、硬截止、预计投入 | PR #28 |
| 展示排序结果和排序理由 | PR #28 |
| 生成今日 1 至 3 个焦点任务 | PR #29 |
| 状态变化后自动重排并展示变化原因 | PR #29 |
| 容量不足时移回规划池 | PR #29 |
| 缺估算、阻塞、硬截止冲突可见 | PR #29 |
| Done 与 Reopen 回流 | PR #29 |

本期不做：

- 不做通用 ToDo。
- 不做团队看板、管理者视图、成员绩效统计。
- 不做多仓库。
- 不全量同步历史 Issue。
- 不写回 GitHub Issue、PR、Label、Milestone、Review、Check。
- 不自动关闭 Issue、合并 PR、改 deadline。
- 不自动猜评论含义。
- 不自动编造预计投入。
- 不做完整日历、时间块、番茄钟、日报。
- 不做黑盒 AI 排序分数。

## 7. 视觉系统

视觉主题：黑白灰工程工作台。

它应该像一个安静、可扫读的 macOS 工作工具。不要像营销页，不要像普通 Todo，也不要像 GitHub 页面换皮。

### 7.1 配色

只使用黑、白、灰。不要任何有明显色相的品牌色或状态色。

| Token | Hex | 用途 |
| --- | --- | --- |
| `canvas` | `#F7F7F8` | 应用背景 |
| `surface` | `#FFFFFF` | 主面板、右侧详情栏 |
| `surface-muted` | `#F1F1F2` | 左侧导航、浅背景 |
| `surface-raised` | `#FAFAFA` | 回执、浮层、表单区域 |
| `text` | `#18181B` | 主文字 |
| `text-secondary` | `#52525B` | 次级文字 |
| `text-muted` | `#8A8A93` | 弱文字、时间、说明 |
| `border` | `#D4D4D8` | 结构线 |
| `border-soft` | `#E4E4E7` | 轻分隔 |
| `ink` | `#27272A` | 主按钮、当前高亮 |
| `ink-hover` | `#3F3F46` | hover / active |
| `ink-soft` | `#EDEDEF` | 选中行、当前节点浅底 |

状态差异通过文字、图标、边框样式和灰阶深浅表达，不使用红、黄、绿、蓝、紫。

### 7.2 字体

| 用途 | 字体 |
| --- | --- |
| 中文 UI | `PingFang SC`, `SF Pro Text`, `Microsoft YaHei`, `-apple-system`, `BlinkMacSystemFont`, `sans-serif` |
| 页面标题 | `SF Pro Display`, `PingFang SC`, `Microsoft YaHei`, `sans-serif` |
| 编号和数字 | `SF Mono`, `JetBrains Mono`, `ui-monospace`, `Menlo`, `Consolas`, `monospace` |

规则：

- 中文不加字距，`letter-spacing: 0`。
- 不用 Inter，不用 serif。
- 不用超大 Hero 字号。
- Issue 编号、PR 编号、时间、容量、版本号用等宽字体。

### 7.3 图标

使用 Lucide outline icons。GitHub logo 只作为 GitHub 来源例外。

| 用途 | 图标 | 中文含义 |
| --- | --- | --- |
| GitHub | `Github` | GitHub 来源 |
| Issue | `CircleDot` | Issue |
| PR | `GitPullRequest` | PR |
| Review | `MessagesSquare` | Review |
| 同步 | `RefreshCw` | 同步 |
| 搜索 | `Search` | 搜索 |
| 排序 | `SlidersHorizontal` | 排序 |
| 证据 | `ExternalLink` | 打开 GitHub |
| 变化 | `ListChecks` | 计划变化 |
| 冲突 | `TriangleAlert` | 容量冲突 |
| 阻塞 | `OctagonAlert` | 阻塞 |
| 已结束 | `Archive` | 已结束 |

不用 emoji，不用大装饰图标。

## 8. 核心组件

### 8.1 任务行

任务行是记录，不是大卡片。

固定内容：

```text
[来源图标] Issue #31  完成登录页
P0 · 截止今天 · 预计 1h · 需处理
原因：PR 收到 requested changes，需要返工
```

规则：

- 默认高度 56px。
- 选中高度 60px，背景 `ink-soft`。
- 每行最多 4 个标签：来源、优先级、截止、预计投入。
- 行尾最多 2 个动作：`查看排序理由`、`打开 GitHub`。
- hover 只改变背景，不做彩色高亮。

### 8.2 状态标签

状态标签使用灰阶 outline，不使用彩色 pill。

| 标签 | 样式 |
| --- | --- |
| 需处理 | 深灰文字，细实线边框 |
| 待评审 | 中灰文字，虚线边框 |
| 已结束 | 浅灰文字，实线边框 |
| 待补估算 | 中灰文字，加 `TimerReset` 图标 |
| 阻塞 | 深灰文字，加 `OctagonAlert` 图标 |
| 容量冲突 | 深灰文字，加 `TriangleAlert` 图标，粗 2px 边框 |

### 8.3 变化回执

变化回执必须是固定区域，不是 toast。没有变化时也保留位置。

```text
计划变化

+ Issue #31 加入今日焦点
  原因：PR 收到 requested changes，需要返工 1h

- Issue #37 移回规划池
  原因：今日剩余容量不足

~ Issue #28 提前
  原因：同优先级内截止更早

! Issue #42 出现容量冲突
  原因：硬截止任务所需 3h，今日剩余 2h
```

`+`、`-`、`~`、`!` 使用等宽字体。所有符号仍用黑白灰，不上色。

### 8.4 排序理由

排序理由必须逐层解释，不出现 AI 分数。

固定层级：

1. 是否 24 小时内硬截止；
2. 优先级 P0 / P1 / P2；
3. 同优先级内截止更早；
4. 预计投入是否能放进今日容量；
5. 无 DDL 是否因长期未处理而上浮；
6. 手动顺序是否被保留。

每一层都要显示原始字段，用户能核对。

### 8.5 表单

- 标签在输入框上方。
- 错误信息在字段下方。
- 预计投入用分段控件：`30m`、`1h`、`2h`、`自定义`。
- 不能自动填入 AI 猜测值。
- 只能保存用户确认后的值。

## 9. 页面规格

### S01. GitHub 授权

目的：说明只读授权，避免用户以为产品会改 GitHub。

主体内容：

- 标题：`连接 GitHub`。
- 主按钮：`连接 GitHub`。
- 权限范围：读取分配给我的 Issue、我的 PR、请求我 Review 的 PR、仓库和 Milestone 元数据。
- 明确边界：不关闭 Issue、不合并 PR、不改 Label、不改 Milestone、不写 deadline。

右侧详情：

- `GitHub 证据`：暂无，等待授权。
- `当前状态`：未连接。
- `排序或计划依据`：暂无。
- `计划变化`：暂无计划变化。
- `可执行动作`：连接 GitHub。

跳转：`连接 GitHub` -> S02。

### S02. 选择仓库与 Milestone

目的：表达 MVP 只绑定一个仓库和当前 Milestone。

主体内容：

- 当前账号：`ZhaoXingPeng`。
- Organization：`1024XEngineer`。
- Repository：`XE6-15`。
- Milestone：当前 MS1 Milestone。
- 其他仓库置灰，说明 `本期不做多仓库`。

主按钮：`使用此仓库`。

右侧详情展示：

- 为什么只选一个仓库；
- RepositoryContext 不包含 Token；
- 后续只把 RepositoryContext 传给 Task Pool。

跳转：`使用此仓库` -> S03。

### S03. 导入与初始化

目的：展示 Task Pool 如何从 GitHub 事实生成个人行动项。

主体内容：

- 步骤条：授权 -> 读取仓库 -> 收集开放任务 -> 初始化本地字段。
- 来源：Assigned Issue、My PR、Review Request。
- 导入结果：已导入、缺预计投入、待评审、阻塞、同步失败来源。
- 部分失败：明确显示失败来源和原因，保留上次成功快照。

右侧详情展示映射规则：

```text
GitHub Issue / PR / Review Request
-> GitHubFacts
-> PersonalAction
-> TaskPoolSnapshot
```

跳转：导入完成 -> S04。

### S04. 我的工作

目的：展示所有与我有关的 PersonalAction，但不等于今日计划。

主体内容：

- 顶部筛选只允许：`全部`、`需处理`、`待评审`、`已结束`。
- 页面内分区可以出现：`待补估算`、`阻塞`、`规划池候选`。
- 至少展示一个 Issue、一个 My PR、一个 Review Request。
- 每项都有 GitHub 原始链接。

不能出现：

- 侧边栏新增 `待评审` 或 `阻塞任务`。
- 自动排序成今日计划。
- GitHub 写回按钮。

主按钮：`进入工作台`。

跳转：`进入工作台` -> S05。

### S05. 工作台总览

目的：主页面。用户一眼看到今天先做什么、为什么，以及最新变化。

主体内容：

- 今日焦点摘要：最多 3 条。
- 我的工作摘要：需处理、待评审、待补估算、阻塞、已结束计数。
- 最新计划变化：显示 1 个回执摘要。
- 选中任务在列表、状态标签、右侧详情同步高亮。

固定主动作：`查看今日焦点`。

跳转：

- `查看今日焦点` -> S06。
- 任务行 `查看排序理由` -> S07。
- 最新变化 -> S09。

### S06. 今日焦点

目的：展示今天真正执行的 1 至 3 个任务。

主体内容：

- 今日容量：总量、已安排、剩余。
- 今日焦点列表：最多 3 条。
- 每条任务有一句原因。
- 待评审、阻塞、待补估算、已结束不占今日容量。

右侧详情：

- 当前任务 GitHub 证据；
- 当前状态；
- 进入今日焦点的计划依据；
- 最新计划变化。

跳转：

- 任务行 -> S07。
- `查看计划调整` -> S09。

### S07. 排序理由

目的：让 PR #28 的排序规则可解释、可评审。

主体内容：

- 选中任务身份：来源、编号、标题、仓库、GitHub 链接。
- 排序理由六层结构。
- 排序原因代码转中文展示，例如 `dueWithin24Hours` 显示为 `24 小时内到期`。
- 缺预计投入时显示 `待补估算`，并说明不能进入今日容量。

不能出现：

- AI 置信度；
- 黑盒分数；
- “系统觉得更重要”这类不可核对文案。

跳转：

- `补充预计投入` -> S09。
- 返回 `今日焦点` -> S06。

### S08. 评审反馈

目的：展示什么 GitHub 反馈会触发任务回到需处理。

主体内容：

- 当前 PR 证据：PR 编号、Reviewer、Review 状态、更新时间。
- 反馈类型：
  - `requested changes`：任务回到需处理；
  - 普通评论：不改变主状态；
  - approved：保持待评审，等待合并。
- 页面说明：产品不猜评论含义，只处理明确状态。

右侧详情：

- GitHub 证据放在第一组；
- 当前状态从 `待评审` 变为 `需处理` 时，计划变化槽显示触发说明。

主按钮：`更新计划`。

跳转：`更新计划` -> S09。

### S09. 计划调整

目的：展示 Daily Planning 的结果，包括变化回执、规划池、待补估算、阻塞和容量冲突。

主体内容分四块，位置固定：

1. `计划变化`：PlanChangeSet 完整回执。
2. `规划池`：今日放不下但仍需处理的任务。
3. `待补估算与阻塞`：两个并列灰阶分区。
4. `容量冲突`：仅在存在冲突时展开，否则显示 `暂无容量冲突`。

容量冲突必须展示：

- 今日剩余容量；
- 必须今日完成所需时间；
- 缺口；
- 冲突任务；
- 可选处理方向：增加今日容量、调整任务范围或预计投入、手动移出一项、接受延期风险。

不能出现：

- 静默延期；
- 自动缩短预计投入；
- 删除被移出任务；
- 写回 GitHub deadline。

跳转：

- `查看今日焦点` -> S06。
- `打开 GitHub` -> 外部链接。

### S10. 已结束与重开

目的：展示 Done 和 Reopen 的边界。

主体内容：

- 已合并 PR；
- 关闭未合并 PR；
- 已关闭 Issue；
- Reopen 示例。

规则：

- PR merged 显示 `已合并`。
- PR closed but not merged 显示 `关闭未合并`。
- Issue 或未合并 PR reopened 后回到 `需处理`。
- Reopen 保留原优先级、硬截止、预计投入和手动顺序，再重新参与排序和计划。

跳转：

- Reopen 事件 -> S09 计划调整。
- 调整完成 -> S06 今日焦点或 S04 我的工作。

## 10. 样例数据

所有页面使用同一组样例数据。不要为截图美观额外编造指标。

原型日期：`2026-07-15`。

| ID | 来源 | 标题 | 状态 | 位置 | 优先级 | 截止 | 预计投入 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T1 | Issue + PR | `Issue #31 完成登录页` | 需处理 | 今日焦点 | P0 | 2026-07-15 | 1h |
| T2 | Issue | `Issue #28 对齐排序规则` | 需处理 | 今日焦点 | P1 | 2026-07-16 | 1.5h |
| T3 | Review | `PR #30 自动导入任务 Proposal` | 需处理 | 今日焦点 | P1 | 2026-07-15 | 1h |
| T4 | PR | `PR #45 完成登录页` | 待评审 | 页面内状态 | P0 | 2026-07-15 | 1h |
| T5 | Issue | `Issue #37 补充错误提示` | 需处理 | 规划池 | P1 | 2026-07-16 | 2h |
| T6 | Issue | `Issue #51 更新项目文档` | 需处理 | 待补估算 | P2 | 2026-07-16 | 缺失 |
| T7 | Issue | `Issue #18 等待 Reviewer 回复` | 需处理 | 阻塞 | P1 | 无 DDL | 2h |
| T8 | Issue | `Issue #42 提交训练营作业` | 需处理 | 容量冲突 | P0 | 2026-07-15 | 2h |
| T9 | PR | `PR #29 自动重排 Proposal` | 待评审 | 页面内状态 | P0 | 2026-07-15 | 1h |
| T10 | PR | `PR #52 旧数据方案尝试` | 已结束 | 已结束 | P2 | 无 DDL | 1h |

## 11. 可点击流程

### 路径 A：首次导入

```text
S01 GitHub 授权
-> S02 选择仓库与 Milestone
-> S03 导入与初始化
-> S04 我的工作
-> S05 工作台总览
```

### 路径 B：查看今日焦点和排序

```text
S05 工作台总览
-> S06 今日焦点
-> S07 排序理由
-> S06 今日焦点
```

### 路径 C：评审反馈触发计划变化

```text
S06 今日焦点
-> S08 评审反馈
-> S09 计划调整
-> S06 今日焦点
```

### 路径 D：异常与回流

```text
S09 计划调整
-> 补充预计投入 / 解除阻塞 / 处理容量冲突
-> S09 计划调整
-> S06 今日焦点
```

### 路径 E：结束与重开

```text
S08 评审反馈
-> PR merged
-> S10 已结束与重开
-> Issue reopened
-> S09 计划调整
-> S06 今日焦点
```

## 12. 动效与交互

动效只服务状态变化。

| 交互 | 时间 |
| --- | --- |
| hover / focus | 120ms ease-out |
| 面板内容切换 | 160ms opacity |
| 变化回执更新 | 180ms opacity + 轻微 translateY |
| 任务行移动 | 220ms transform + opacity |
| reduced motion | 只保留 opacity |

规则：

- 只动画 transform 和 opacity。
- 不动画 width、height、top、left。
- 不用 bounce、confetti、漂浮闲置动效。
- 任务移动不能造成文字重叠。

快捷键：

- `Cmd K`：命令。
- `Cmd R`：同步。
- `Enter`：打开选中任务详情。
- `Esc`：关闭浮层。
- 方向键：移动任务行焦点。

## 13. 状态覆盖

每个页面至少覆盖默认状态。相关页面必须覆盖下列状态：

| 状态 | 页面 | 处理方式 |
| --- | --- | --- |
| 加载 | S03 | skeleton 行匹配最终布局，不用圆形 spinner 作为主状态。 |
| 空 | S04 / S06 | 说明为什么为空，并提供下一步。 |
| 错误 | S01 / S03 | 显示具体原因和重试方式。 |
| 部分成功 | S03 | 成功导入内容仍可见。 |
| 同步失败 | S05 | 显示上次成功快照，不伪装成无任务。 |
| 计划变化 | S05 / S06 / S09 | 固定回执槽展示。 |
| 容量冲突 | S09 | 专门区域展示，不用 toast。 |

## 14. Stitch 生成顺序

为了先固定组件，不要按首次使用流程生成。按这个顺序：

1. S05 工作台总览：先固定全局 shell、顶部栏、侧边栏、右侧详情和回执槽。
2. S06 今日焦点。
3. S09 计划调整。
4. S07 排序理由。
5. S08 评审反馈。
6. S10 已结束与重开。
7. S04 我的工作。
8. S03 导入与初始化。
9. S02 选择仓库与 Milestone。
10. S01 GitHub 授权。

生成完后再把点击流按 S01 -> S10 串起来。

## 15. 验收检查

接受 Stitch 输出前逐项检查：

| 检查项 | 通过标准 |
| --- | --- |
| 页面数量 | exactly 10 screens，S01 到 S10。 |
| 平台 | macOS 桌面窗口，默认 1440 x 960。 |
| 固定组件 | 顶部工具栏、左侧导航、标题区、状态轨道、右侧详情栏、回执槽每页都存在。 |
| 左侧导航 | 只出现 6 个固定入口，不出现等待评审、阻塞、待估算、规划池等随机 Tab。 |
| 中文 | 页面标题、按钮、导航、状态说明为中文；GitHub / Issue / PR / Review / Milestone 保留英文。 |
| 配色 | 只使用黑白灰，不使用绿、蓝、红、紫或渐变。 |
| 功能来源 | 只覆盖 PR #30、PR #28、PR #29 和 arch.md 数据流。 |
| 数据流 | 页面不绕过模块边界，不暗示写回 GitHub。 |
| 解释性 | 排序和计划变化都有可核对原因。 |
| 密度 | 每屏一个主动作，今日焦点最多 3 条。 |
| 可访问性 | 对比度足够，焦点顺序清楚，图标按钮有 label，文字不溢出。 |

## 16. Stitch Prompt

和本文件一起上传给 Stitch：

```text
请根据 DESIGN.md 生成 GitDay 的中文 macOS 桌面端原型。必须生成 exactly 10 screens：S01 GitHub 授权、S02 选择仓库与 Milestone、S03 导入与初始化、S04 我的工作、S05 工作台总览、S06 今日焦点、S07 排序理由、S08 评审反馈、S09 计划调整、S10 已结束与重开。

先生成 S05，固定所有全局组件：macOS 标题栏、56px 顶部工具栏、216px 左侧导航、72px 主标题区、56px 模块状态轨道、340px 右侧详情栏、固定变化回执槽。随后生成其他页面，所有页面都必须保留这些组件，不允许页面之间更换顶部栏、侧边栏或右侧详情结构。

左侧导航只允许：工作台、我的工作、今日焦点、评审反馈、计划调整、已结束。不要生成等待评审、阻塞任务、待估算、规划池、Dashboard、Inbox、Settings 等额外 Tab。

界面文案优先中文。仅保留必要英文技术对象：GitHub、Issue、PR、Review、Milestone、OAuth、Token、Cmd K。不要出现英文营销文案。

视觉只使用黑白灰：#F7F7F8、#FFFFFF、#F1F1F2、#18181B、#52525B、#8A8A93、#D4D4D8、#E4E4E7、#27272A。不要使用 GitHub 绿、蓝色链接、红色警告、紫蓝渐变、霓虹或玻璃拟态。状态差异通过文字、图标、边框和灰阶表达。

功能和数据流只来自 PR #30 自动导入任务、PR #28 排序与展示、PR #29 自动重排，并遵守 arch.md：GitHub Connection -> Task Pool -> Prioritization -> Daily Planning，由 Workspace Coordinator 串联。不要表现为 UI 直接写回 GitHub，也不要让排序模块直接改今日计划。

每个页面都必须是真实产品状态。每次计划变化必须在固定回执槽展示原因。不要生成普通 ToDo、营销首页、组件库页或额外页面。
```

## 17. 禁止模式

Never generate：

- 10 个以外的页面数量；
- 手机端或平板端；
- 营销 hero；
- 普通 checklist ToDo；
- 彩色主题、GitHub 绿主品牌、红黄绿状态色、蓝紫渐变；
- 侧边栏随机 Tab；
- 大面积英文；
- emoji；
- 假效率指标、假 uptime、假 AI confidence score；
- 黑盒 AI 排序；
- 自动写回 GitHub；
- toast 承载关键计划变化；
- 静默重排、静默延期、自动删除任务；
- 卡片套卡片、装饰 blob、玻璃拟态、霓虹外发光。
