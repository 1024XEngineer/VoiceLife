# VoiceLife 文档导航

本目录只保留当前版本的使用、维护和架构约束。历史方案、一次性实验和评审过程以对应 Issue、PR 和 Git 历史为准；不要把它们重新复制到 `docs/`。

| 读者 | 入口 |
| --- | --- |
| 使用者与新贡献者 | [项目 README](../README.md)、[参与开发](../CONTRIBUTING.md) |
| 硬件调试与烧录 | [硬件调试与日志规则](engineering/hardware-debugging.md)、[SparkBot 刷写与资源清单](engineering/sparkbot-flash-and-assets.md)、[SQLite 实板验证与恢复](engineering/board-storage-validation.md)、[百炼与 SparkBot 语音实板验证](engineering/bailian-sparkbot-voice-validation.md) |
| 语音日程联调 | [语音日程联调测试矩阵](engineering/schedule-voice-integration-test-matrix.md)、[语音日程边界调研与本期取舍](engineering/schedule-voice-industry-boundaries.md) |
| 架构维护者 | [架构与适配器设计](architecture/design-guidelines.md)、[语音子架构](architecture/voice-subarchitecture.md)、[SQLite 存储子架构](architecture/storage-subarchitecture.md)、[ADR](adr/) |
| 组件与服务维护者 | [SparkBot 显示组件](components/sparkbot-display.md)、[IM Gateway](services/im-gateway.md) |
| 协作与交付 | [协同开发](engineering/collaboration.md)、[质量门禁](engineering/ci-quality-gates.md)、[契约版本](engineering/contract-versioning.md)、[提交规范](engineering/commit-convention.md) |
| 文档归档判断 | [文档放置规则](engineering/document-placement.md)、[历史归档](archive/README.md) |

Issue [#264](https://github.com/1024XEngineer/VoiceLife/issues/264) 将旧小智能力迁移计划和旧 ESP32-S3 验证记录移入[历史归档](archive/README.md)，并删除没有独立长期价值的 SparkBot 阶段执行草稿。语音原始研究材料继续归档在 Issue [#150](https://github.com/1024XEngineer/VoiceLife/issues/150)。
