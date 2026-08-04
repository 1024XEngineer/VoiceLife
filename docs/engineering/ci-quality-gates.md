# VoiceLife CI 与提交前质量门禁

这份规范把“提交前要检查什么”变成一条可以复制执行的命令，并说明每道门禁真正保护的风险。结论很简单：代码进入 PR 前先运行 `./scripts/run_pre_submit_checks.sh`，CI 再用并行任务重复验证；任何门禁被跳过，都必须在 PR 中写出原因和替代证据。

## 1. 一条命令跑完提交前检查

在最新 `main` 基线上开发，并先同步远端：

```bash
git fetch origin main
./scripts/run_pre_submit_checks.sh
```

脚本会按以下顺序执行：

1. C/C++ 使用 `.clang-format`，Python 使用 Ruff，检查只读格式和静态规则。
2. 检查本次变更源码的文件规模。
3. 检查公共 C++ API 文档、主机测试、架构依赖、Profile 和 Python 测试。
4. 使用仓库 `package.json` 声明的 pnpm 版本，运行 IM Gateway 的 Prettier、ESLint、TypeScript 和测试。

本地工具版本要和 CI 对齐：clang-format 使用 18，Ruff 使用 `0.12.7`，gcovr 使用 `8.5`，IM Gateway 使用 Node.js 24 和锁定的 pnpm 版本。缺少工具时让脚本失败，不要用“本机没装所以跳过”代替验证。

只想缩短反馈时，可以先运行 `./scripts/run_host_tests.sh -R <test-name>`；这只是 TDD 内循环，不能替代完整门禁。

## 2. CI 任务和合并含义

| 任务 | 保护的风险 | 合并前要求 |
| --- | --- | --- |
| 工作流语法检查 | YAML、表达式和 job 配置写错后才暴露 | 必须通过 `actionlint` |
| 格式与 Python 静态检查 | 代码风格漂移、明显的 Python 错误 | 必须通过 clang-format 与 Ruff |
| IM Gateway | TypeScript 类型、Lint、格式和契约回归 | 必须通过冻结依赖安装和 `pnpm run ci` |
| 覆盖率（Codecov） | C++ 与 TypeScript 代码路径长期退化 | C++ 与 TypeScript 项目/改动行目标为 80%，报告上传失败直接失败 |
| 主机测试与架构边界 | 领域行为回归、组件反向依赖 | 必须通过 CTest、Profile 校验和架构检查 |
| 变更源码规模 | 新模块过大、职责无法继续拆分 | 新增源码文件超过 500 行直接失败；现有文件超过 800 行给出警告并要求在 Review 中说明 |
| ESP-IDF 构建 | 头文件和组件依赖在目标板上无法编译 | 必须通过 ESP-IDF 6.0.2 / ESP32-S3 构建 |
| 依赖变更审查 | PR 引入已知漏洞或不允许的许可证变化 | Dependency Graph 已启用时必须通过；未启用时必须在日志中留下跳过原因 |
| CodeQL | C/C++、Python、TypeScript 的数据流和安全缺陷 | 必须通过对应语言扫描 |

CI 将第三方 Action 固定到完整 commit SHA，并在同一行保留版本注释；Dependabot 负责提出升级 PR。工作流默认只申请 `contents: read`，需要额外权限的 job 必须把权限写在 job 级别，并在 PR 中说明用途。

依赖审查有一个仓库级前提：GitHub 的 Dependency Graph 必须开启。工作流会先读取仓库的 SBOM 接口；返回 `404` 时只记录 warning 并保持 job 可合并，避免把“仓库能力未开”误报成代码失败。管理员开启 Dependency Graph 后，同一 job 会自动执行 `actions/dependency-review-action`；真实的漏洞或许可证违规仍会让 job 失败，不能用 `continue-on-error` 静默吞掉。

当前 VoiceLife 是公开仓库，使用 GitHub 托管的 `ubuntu-24.04`。仓库改为私有且组织要求迁移时，才切换到七牛文档约定的 Runner label；切换前必须用一次真实 job 证明 Runner 已注册并接单。AK、SK、SSH_KEY 和任何 AI 服务变量都不能写进 workflow，当前项目也不读取它们。

## 3. 格式规则

- C/C++ 文件使用 `.cc` / `.h`，由 `.clang-format` 统一检查；不要在业务 PR 里手工混入另一套缩进。
- Python 使用 Ruff 的 `E/F/I/UP/B/SIM` 规则和 Ruff formatter；导入排序、可疑分支和明显重复由工具处理。
- IM Gateway 使用 Prettier、ESLint 和 TypeScript 编译检查；锁文件必须与 `package.json` 同步，CI 使用 `--frozen-lockfile`。
- 机械格式化和行为变更默认拆成两个 PR。确实需要一次性建立格式基线时，PR 正文必须给出“先看行为、再看格式”的阅读顺序，并提供格式化前后行为测试证据。

## 4. 注释与公共 API 文档

注释服务于读代码的人，不是为了把每行代码翻译成中文。

- `components/**/include/**/*.h` 中的公共类型使用紧邻的 `///` 或 `/** ... */` 中文说明。
- 公共函数使用 `/** ... */`，必须包含 `@brief`；有参数写全 `@param`，有非 `void` 返回值写 `@return`。
- 注释优先解释错误语义、所有权、时间单位、并发约束、幂等和降级条件；函数名和字段名已经表达清楚的内容不要重复抄一遍。
- 私有实现只在“为什么这样做”不明显时写注释。临时方案使用 `TODO(#Issue)`，必须说明删除条件；禁止留下没有 owner 的 TODO。
- 迁移上游代码时保留上游链接、commit 和许可证；不要把上游英文注释原样当作本项目公共 API 规范。

`scripts/check_public_api_docs.py` 是硬门禁，但它是启发式扫描器，不替代 Reviewer 对语义和注释质量的判断。新增声明语法复杂时，先补测试再扩展扫描器。

## 5. 架构和代码规模

代码规模是风险信号，不是为了追求某个漂亮数字：

- 新增源码文件默认不超过 500 行；超过即拆分职责，或在 Design/ADR 中写清为什么不能拆。
- 现有源码文件超过 800 行由 CI 警告，不在一次格式化 PR 里强行重构；后续功能 PR 必须逐步降低规模。
- 单个函数超过 80 行需要 Reviewer 明确判断，超过 120 行原则上拆成可命名的策略或 Port；循环嵌套、分支数量和参数数量比物理行数更值得关注。
- `components/` 的依赖方向由 `scripts/check_architecture.sh` 维护：Domain 不认识 Application/Adapter，跨组件只能引用公共 `include/`，平台 SDK 只留在 Adapter。
- 新增模块必须同时补公开接口、CMake 依赖、主干调用和契约测试；只增加目录或只增加“未来接口”不算架构完成。

规模警告不是对现有债务的豁免。`services/im-gateway/src/application/services.ts` 等历史大文件保留在基线中，但新增功能不得继续把它们变成万能服务。

## 6. TDD、PR 和 Review

- 新行为先写能失败的测试，PR 记录 RED 的真实失败原因、GREEN 的最小实现和 REFACTOR 的范围。
- PR 必须关联 Issue，结论先行，写清未包含范围、验证证据、已知风险和回退方式。
- CI 绿不等于自动合并。还需要非作者 Review；AI Review 只能提供线索，不能替代人工批准。
- 测试通过但真实硬件、网络、掉电或外部服务未验证时，PR 必须明确写“主机/Mock 已通过，真实环境未覆盖”。

## 7. 规则变更

修改格式、注释、门禁阈值或工作流时，先改这份规范和对应测试，再改脚本；规则变更本身也要经过同样的 CI 与 Review。阈值如果阻塞已有代码，应先建立基线和迁移计划，不要为了让 CI 变绿而静默删除检查。

## 8. 依据

本规范在 2026-08-04 复核了以下公开资料，并按 ESP32 组件化项目的规模收敛：

- GitHub Actions 安全使用：[Secure use reference](https://docs.github.com/en/actions/reference/security/secure-use)
- GitHub 依赖审查：[About dependency review](https://docs.github.com/en/code-security/concepts/supply-chain-security/dependency-review)
- GitHub CodeQL：[About CodeQL code scanning](https://docs.github.com/en/code-security/code-scanning/introduction-to-code-scanning/about-code-scanning-with-codeql)
- GitHub Actions Runner：[Using self-hosted runners in a workflow](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/use-in-a-workflow)
- Google Engineering Practices：[Small CLs](https://google.github.io/eng-practices/review/developer/small-cls.html)
- LLVM clang-tidy：[readability-function-size](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-size.html)
- ESLint：[max-lines](https://eslint.org/docs/latest/rules/max-lines)
- Ruff：[Lint rules](https://docs.astral.sh/ruff/linter/)
- actionlint：[项目说明](https://github.com/rhysd/actionlint)
