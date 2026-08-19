# 参与 VoiceLife 开发

这个仓库先守边界，再填功能。开始编码前，请先确认改动对应一个可验收的 Issue，并判断它属于领域、用例还是适配器；PR 需要同时说明验证结果和没有覆盖的风险。

## 开发流程

> **核心规则：所有人都通过 Fork 工作流提交 PR，禁止直接在主仓库（upstream）上建分支或推送。**
> 主仓库仅维护 `main` 分支；所有个人工作分支都应在自己的 Fork 上创建、推送和发起 PR。

1. **Fork 主仓库**：首次参与开发前，在 GitHub 上 Fork `1024XEngineer/VoiceLife` 到你的个人账号。将你的 Fork 添加为 `origin`，主仓库添加为 `upstream`：
   ```bash
   git remote add upstream git@github.com:1024XEngineer/VoiceLife.git
   git remote set-url origin git@github.com:<你的用户名>/VoiceLife.git
   git remote -v  # 确认 origin → 你的 Fork，upstream → 主仓库
   ```
2. 在当前 Milestone 下创建或领取 Issue，写清场景、范围和验收标准。
3. 涉及边界、接口、数据模型或依赖方向时，先提交 Design Issue；重大取舍补 ADR。
4. **从上游最新 `main` 创建个人分支**（始终基于 upstream 保持同步）：
   ```bash
   git fetch upstream
   git switch -c dev/<issue>-<short-name> upstream/main
   ```
   命名规范：`dev/<issue>-<short-name>`，例如 `dev/91-tdd-architecture`。仓库不维护长期共享的裸 `dev` 分支。
5. 先写失败测试并记录 RED 原因，再补最小实现使其 GREEN；重构期间保持测试通过。
6. 小步提交。每个提交只表达一个可回退的意图，并保持可编译。
7. 本地运行 `./scripts/run_pre_submit_checks.sh`；设备相关改动还要运行对应 Profile 构建和真机检查。
8. 将分支推送到**你自己的 Fork（origin）**，然后发起 PR：
   ```bash
   git push -u origin HEAD
   # 在 GitHub 上打开 PR，目标分支选 upstream/main
   gh pr create --title "📝 docs(readme): 补充贡献者指南" --body "..."
   ```
9. PR 使用中文写结论、TDD 记录、验证和风险，关联 Issue，等待 CI 与 Review 通过后合并。

**禁止行为**：
- ❌ 在主仓库（upstream）上直接创建或推送分支。
- ❌ 将分支推送到 `origin` 后不发起 PR，直接要求合入。
- ❌ 以 `main` 以外的长期共享分支作为工作基线。

`main` 始终保持可构建、可回退。只有多个任务确实需要联合验证时，才临时建立 `integration/<milestone>-<topic>`；联调结束后通过一个 PR 合回 `main` 并删除该分支，不能把它变成第二条长期主线。

阶段性交付在 PR 中写 `Refs #123`；只有一个 PR 已完成 Issue 的全部验收时才写 `Closes #123`。普通单一交付默认使用 Squash Merge，保持 `main` 简洁；初始化架构等包含多笔可独立审查提交的 PR 可以使用 Merge commit。合并后删除个人 fork 上的任务分支。

## TDD 快速循环

新增行为时，先把 Issue 的验收条件写成一个能失败的测试。用测试名缩小反馈范围：

```bash
# RED：先确认测试因缺少目标行为而失败
./scripts/run_host_tests.sh -R schedule_policy_test

# GREEN / REFACTOR：反复运行同一个测试
./scripts/run_host_tests.sh -R schedule_policy_test

# 提交前跑完整门禁（格式、规模、主机、架构、固件配置和 IM Gateway）
./scripts/run_pre_submit_checks.sh
```

领域规则、Application 和协议映射优先在主机测试完成。真实 Codec、网络重连、掉电恢复等硬件行为使用 ESP-IDF Unity 测试与真机证据，不能用主机 mock 冒充设备通过。

## 架构底线

- 领域组件不能包含 ESP-IDF、HTTP、XRobot、Koishi、微信或飞书类型。
- MCP 只校验和路由，不持有日程状态。
- Voice 只编排会话、音频和工具调用，不执行日程业务。
- Schedule 与 TimingTask 分开；需要一起提交时，通过原子存储 Port 完成。
- 新平台通过 Adapter 接入，不能在核心增加平台名称分支。
- Profile 只能引用凭据，不能保存 token、密码、设备身份或用户隐私。

完整规则见 [架构与适配器设计规范](./docs/architecture/design-guidelines.md)。

## 常用检查

```bash
# 只检查 C/C++、Python 格式与静态规则（需 clang-format 和 ruff）
./scripts/check_format.sh

# 公共 C++ API 文档、主机测试、架构边界、固件配置与 Python 测试
./scripts/run_checks.sh

# IM Gateway 的格式、Lint、类型与测试
pnpm install --dir services/im-gateway --frozen-lockfile
pnpm --dir services/im-gateway run ci

# 覆盖率：CI 使用 GCC/gcovr 与 c8 上传到 Codecov
# 本机生成 C++ 覆盖率需要 GCC 和 gcovr
python3 scripts/firmware.py build esp32s3-dev
```

公共 C++ API 位于 `components/**/include`。类型和枚举使用简洁的 `///` Doxygen 注释；公开函数必须使用 `/** ... */`，包含 `@brief`，并为每个参数添加 `@param`、为每个非 `void` 返回值添加 `@return`。注释应说明职责和签名无法表达的语义（例如错误、所有权、时间单位或并发约束），不要为私有实现添加重复代码的注释。

IM Gateway 的 TypeScript 导出 API 必须使用包含中文职责说明的 `/** ... */` TSDoc。导出函数、导出接口的方法和导出类的公开成员还必须为参数和非 `void` 返回值添加 `@param` 与 `@returns`；实现方法可通过 `{@inheritDoc Interface.method}` 继承接口契约，`private` 和 `protected` 成员除外。`pnpm --dir services/im-gateway run docs:check` 会全量检查所有源码，不放行未修改的历史声明。

提交前可手动检查描述：

```bash
python3 scripts/check_commit_message.py --file .git/COMMIT_EDITMSG
```

提交格式、允许使用的 Gitmoji 和完整示例见 [提交描述规范](./docs/engineering/commit-convention.md)。

格式、注释、代码规模、工作流安全和 CI 任务的完整解释见 [CI 与提交前质量门禁](./docs/engineering/ci-quality-gates.md)。
