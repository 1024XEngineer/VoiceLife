# E2E Journey 模板

新增 journey 前复制本页到对应 Issue/PR，并在代码中复用 `scripts/run_e2e.py` 的统一生命周期。

## 标识

- Journey：`<lowercase-name>`
- 层级：`host` / `hil`
- Profile：`host` / `sparkbot` / `pcb`
- 负责人和依赖：`<owner>`, `<service/device>`

## 生命周期

| 阶段 | 资源与动作 | 超时 | 清理 | 失败分类 |
| --- | --- | --- | --- | --- |
| prepare | 租约、临时目录、进程或设备检查 | `<seconds>` | `<action>` | configuration/device/infrastructure |
| run | 真实请求或串口旅程 | `<seconds>` | `<action>` | external/product/device |
| assert | 有序状态和终态断言 | `<seconds>` | `<action>` | product |
| collect | 只采集 allowlist 字段 | `<seconds>` | `<action>` | infrastructure |
| cleanup | 撤销凭据、复位设备、释放租约 | `<seconds>` | `<action>` | cleanup |

## Evidence

- 公开字段：`<ids/timestamps/metrics/markers>`
- 明确禁止：原始串口、token、密码、SSID、个人数据、完整 URL 或命令输出
- 故意失败测试：`<test name>`
- 脱敏校验：`python3 scripts/check_e2e_artifacts.py <artifact-dir>`

## 门禁入口

- 本地：`<exact command>`
- PR / nightly / manual：`<workflow and job>`
- artifact retention：`<days>`
- required check 计划：先观察，达到稳定门槛后再提议

## 验收

- [ ] Host 和 HIL（如适用）使用同一 runner/evidence schema
- [ ] 硬超时、默认 retries=0、并发隔离已测试
- [ ] 无设备、外部服务和产品断言能分别分类
- [ ] 失败与清理 evidence 可上传且无敏感字段
- [ ] 本地命令、workflow 命令和文档一致
