# 参与 VoiceLife 开发

这个仓库先守边界，再填功能。开始编码前，请先确认改动对应一个可验收的 Issue，并判断它属于领域、用例还是适配器；PR 需要同时说明验证结果和没有覆盖的风险。

## 开发流程

1. 在 MS3 下创建或领取 Issue，写清场景、范围和验收标准。
2. 涉及边界、接口、数据模型或依赖方向时，先提交 Design Issue；重大取舍补 ADR。
3. 从 `main` 创建个人分支，命名为 `<github-id>/<issue>-<short-name>`。
4. 小步提交。一个提交只表达一个可回退的意图。
5. 本地运行 `./scripts/run_host_tests.sh`；设备相关改动还要运行对应 Profile 构建和真机检查。
6. PR 使用中文写结论、变更、验证和风险，关联 Issue，等待 Review 后合并。

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
./scripts/run_host_tests.sh
python3 scripts/firmware.py validate
python3 scripts/firmware.py build esp32s3-dev
```

提交前可手动检查描述：

```bash
python3 scripts/check_commit_message.py --file .git/COMMIT_EDITMSG
```

提交格式、允许使用的 Gitmoji 和完整示例见 [提交描述规范](./docs/engineering/commit-convention.md)。
