# VoiceLife 协同开发规范

VoiceLife 采用稳定 `main` 和短任务分支，不维护长期共享的裸 `dev`。日常路径固定为 Milestone → Proposal/Design Issue → `dev/<issue>-<topic>` → Coding PR → Review → Merge → Release；任何进入 `main` 的行为变化都要留下测试和 Review 证据。

## 1. 工作对象和真相来源

| 对象 | 负责回答 | 完成标志 |
| --- | --- | --- |
| Milestone | 本阶段为什么做、截止到哪里 | 范围、日期、最低交付和验收清楚 |
| Proposal Issue | 值不值得做、做什么、不做什么 | 产品判断和验收被接受 |
| Design Issue | 模块、接口、数据和风险怎么处理 | 边界和取舍能指导编码 |
| Coding Issue | 这一个可交付切片怎么完成 | Given/When/Then 或等价验收可执行 |
| PR | 代码是否按 Issue 完成 | CI 通过、Review 结论和风险留档 |
| ADR | 为什么改变长期架构 | 一个决定、备选方案和后果入库 |
| Release | 哪些用户可见变化可以交付 | Tag、Changelog、固件 manifest 和回退方式齐全 |

Issue/PR 是协作和决策记录，代码、测试、Profile Schema 和接口头文件是实现真相。长篇架构说明不能覆盖已经合并的接口语义；冲突时先修文档，再继续开发。

## 2. Issue 规则

- 一个 Issue 对应一个可独立验收、可在一个短周期内完成的结果。
- 标题写结果，不写“研究一下”“完善功能”“相关优化”。
- 必须关联 Milestone；子任务在正文引用父 Issue。
- 功能 Issue 写真实角色和场景、范围内/外、验收标准。
- Design Issue 写候选方案、选择依据、接口/数据影响、风险和迁移路径。
- Bug 写环境、最小复现、期望/实际、日志脱敏说明和回归范围。
- 需求尚未决定时不能直接开 Coding PR。

仓库提供四个 Issue Form。表单字段是最低信息，不限制作者补充更具体的证据。

## 3. 分支、TDD 和提交

分支从最新 `main` 创建：

```text
dev/<issue>-<short-name>
```

例如 `dev/123-xrobot-adapter`。这里的 `dev/` 是任务分支命名空间，不是共享分支。分支应短期存在，同一分支不要混入第二个 Issue；合并后删除，不从旧任务分支继续开发下一项功能。

任务分支必须以主仓库最新 `main` 为基线，并推送到个人 fork：

```bash
git fetch origin
git switch -c dev/123-xrobot-adapter origin/main
git push -u fork HEAD
```

这里的 `origin` 和 `fork` 是本仓库当前远端角色，判断依据始终是 URL，不是远端名字。禁止直接推送主仓库 `main`，也不要从个人 fork 中滞后的 `main` 开始任务。

默认不建立 `develop` 或裸 `dev` 长期分支。它们会让未完成工作积压在一起，也会把一次发布变成大批量合并。只有两个以上任务确实需要联合验证时，才建立 `integration/<milestone>-<topic>`：各任务仍通过 PR 进入集成分支，联调完成后再由一个 PR 合回 `main`，随后删除集成分支。

行为开发使用 Red → Green → Refactor：

1. **RED**：从 Issue 验收条件写出失败测试，确认它是因为目标行为尚未实现而失败，不是环境或语法错误。
2. **GREEN**：只写让当前测试通过的最小实现，同时运行相关回归测试。
3. **REFACTOR**：整理命名、重复和边界，测试必须一直为绿。
4. **FULL CHECK**：提交前运行 `./scripts/run_pre_submit_checks.sh`；硬件行为另附 Unity、pytest-embedded 或真机证据。

为了让提交可检出和回退，仓库不要求保存一个编译失败的 RED commit。作者要在 PR 的 TDD 记录里写清测试名、RED 失败原因、GREEN 结果和重构范围。提交遵循 [Gitmoji 中文提交规范](./commit-convention.md)，每个提交仍应可编译、测试和回退。

## 4. PR 规则

PR 开头先写结论和请求 Reviewer 判断的事项，再写背景。最低内容：

- 阶段性交付写 `Refs #Issue`；只有完成 Issue 全部验收时写 `Closes #Issue`；
- 改了什么，以及明确没有改什么；
- 是否改变 Port、Profile、数据模型、协议或依赖方向；
- 自动测试、ESP-IDF 构建和真机证据；
- 已知风险、兼容窗口和回退方式；
- 迁移上游代码时的 commit、文件、许可和改写点。

控制 PR 大小。结构移动、机械格式化、依赖升级和行为变化尽量拆开。大 PR 无法拆时，在正文给 Reviewer 一条明确阅读顺序。

草稿 PR 可以用于提前对齐，但不能长期代替 Design Issue。没有验收标准、CI 失败或含真实凭据的 PR 不进入 Review。

普通单一交付默认使用 Squash Merge，PR 标题必须符合仓库的中文 Gitmoji 提交规范。初始化架构、迁移工具等确有多笔独立审查价值时使用 Merge commit，保留每笔已通过检查的提交。日常不使用 Rebase Merge，避免提交重写后削弱 PR 边界；合并完成后删除个人 fork 上的任务分支。

## 5. Review 标准

Reviewer 先判断行为和边界，再看代码风格：

1. 是否真的完成关联 Issue，是否偷偷扩大范围；
2. 领域事实和所有权是否清楚；
3. 依赖是否仍然向内，外部类型是否越界；
4. 幂等、原子性、并发、时间和掉电恢复是否被处理；
5. 安全、隐私、凭据和日志是否合规；
6. 测试是否覆盖失败路径，而不是只跑快乐路径；
7. 配置、迁移和回退是否可执行；
8. 代码是否足够简单，让下一位同学能继续开发。

评论使用以下意图词，减少误解：

- `blocking:` 合并前必须解决；
- `question:` 需要作者解释，未必要求改；
- `suggestion:` 非阻塞改进；
- `nit:` 纯风格问题；
- `praise:` 值得保留的做法。

技术事实优先于偏好。作者不能只回“已修改”，要说明修改位置或不采纳理由。Reviewer 批准的是当前 commit；关键代码变化后需要重新 Review。

## 6. CI 合并门槛

当前必须通过：

- PR 内提交描述、C/C++ 与 Python 格式和静态规则检查；
- IM Gateway 的 Prettier、ESLint、TypeScript 与测试；
- Profile Schema/语义校验、纯 C++ 主机测试和组件依赖方向检查；
- C++ 与 TypeScript 覆盖率门禁；
- ESP-IDF 6.0.2 / ESP32-S3 构建和 CodeQL 扫描。

依赖变更审查会先确认仓库是否开启 Dependency Graph；未开启时必须在 job 日志中明确记录跳过原因，开启后按同一门禁执行。

本地完整入口是 `./scripts/run_pre_submit_checks.sh`。`./scripts/run_checks.sh` 只负责公共 API、主机测试、架构、Profile 和 Python 测试；开发中的单测可以用 `./scripts/run_host_tests.sh -R <test-name>` 缩小范围。格式、IM Gateway 和代码规模也属于提交门禁，不能只跑局部测试。

门禁的任务分工、注释规则、代码规模阈值和 Action 安全要求见 [CI 与提交前质量门禁](./ci-quality-gates.md)。

涉及硬件的 PR 还要在正文记录板卡、固件 hash、操作步骤和结果。涉及外部服务的 PR 要区分 mock、沙箱和真实环境，不能把 mock 通过写成端到端通过。

当前仓库是公开仓库，CI 使用 GitHub 托管的 `ubuntu-24.04`。只有仓库转为私有，且组织仍要求迁移时，才按七牛接入文档改用 `github-runner-ubuntu-24-04`；切换前必须由组织管理员确认 GitHub App 已覆盖本仓库、Repository readiness 和 Runner policy 正常，并用一次实际 job 证明 Runner 已注册和接单。

七牛 AK/SK 只用于 S3 缓存，不负责 Runner 注册。只有 job 已接入文档约定的缓存脚本时，才通过 GitHub Secrets 提供 `RUNNER_S3_AK`、`RUNNER_S3_SK`；`SSH_KEY` 也只在拉取私有 Go module 时配置。当前项目没有这些需求，CI 不读取上述 secret。

CI 不使用未说明用途的 secret，历史遗留的 AI 变量也不得在新工作流引用。将来接入 AI Review 时，必须先建立供应商无关变量名、最小权限、输出脱敏、成本上限和“AI 意见不替代人工批准”规则。

## 7. 发布与变更记录

- 版本遵循 Semantic Versioning；`0.x` 阶段仍要记录不兼容变化。
- `CHANGELOG.md` 只写用户和集成人能感知的 Added、Changed、Deprecated、Removed、Fixed、Security。
- 每个 Release 使用受保护 Tag，固件包带 Profile、版本、SHA-256 和构建 manifest。
- 发布前冻结 Profile Schema、持久化格式和对外协议；必须准备上一版本回退路径。
- 构建产物通过 Release/Artifact 分发，不提交到 Git。

## 8. 安全与 AI 使用

- 真实 token、设备备份、用户日程、串口完整日志和 `.env` 不进入 Issue、PR、测试 fixture 或模型上下文。
- 使用 AI 生成代码时，作者仍对接口、许可、测试和可解释性负责。
- 未理解的代码不能提交；“模型建议如此”不是设计依据。
- AI Review 只能提出建议，不能自动合并、自动关闭安全问题或替代导师 Review。
- 发现漏洞按 [SECURITY.md](../../SECURITY.md) 私密报告，不开公开 Issue。

## 9. 会议和进度记录

日报只记录可以验证的变化：Issue/PR 链接、已通过检查、阻塞和下一步。周会围绕 Milestone 偏差、关键风险、需要导师决定的取舍和下周可验收结果，不复述所有 commit。

架构调整当天补 Design/ADR；不要等到 MS 结束后凭记忆补文档。

## 10. 行业实践来源

本规范在 2026-08-03 核对以下来源，并按训练营与嵌入式项目规模做了收敛：

- GitHub Docs：[Setting guidelines for repository contributors](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/setting-guidelines-for-repository-contributors)
- GitHub Docs：[GitHub Flow](https://docs.github.com/en/get-started/using-github/github-flow)
- GitHub Docs：[Syntax for issue forms](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/syntax-for-issue-forms)
- GitHub Docs：[Adding a security policy](https://docs.github.com/en/code-security/getting-started/adding-a-security-policy-to-your-repository)
- GitHub Docs：[Using self-hosted runners in a workflow](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/use-in-a-workflow)
- 七牛：[GitHub Actions 沙箱 Runner 迁移接入](http://las-wiki.qiniu.io/%E6%8C%87%E5%8D%97/GitHub-Actions-%E6%B2%99%E7%AE%B1-Runner-%E8%BF%81%E7%A7%BB%E6%8E%A5%E5%85%A5.md)
- Google Engineering Practices：[The Standard of Code Review](https://google.github.io/eng-practices/review/reviewer/standard.html) 与 [Small CLs](https://google.github.io/eng-practices/review/developer/small-cls.html)
- CMake：[CTest 命令行手册](https://cmake.org/cmake/help/latest/manual/ctest.1.html)
- Espressif：[ESP-IDF 6.0.2 Unit Testing](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-guides/unit-tests.html)
- [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)
- [Semantic Versioning 2.0.0](https://semver.org/)
- [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/)

没有照搬企业级审批矩阵、强制多人 CODEOWNERS 和复杂发布列车。当前团队没有可靠的所有者名单，伪造 `CODEOWNERS` 只会制造错误安全感；确认模块负责人后再增加。
