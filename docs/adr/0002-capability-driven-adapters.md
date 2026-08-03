# ADR 0002：采用能力驱动的适配器 Profile

- 状态：Accepted
- 日期：2026-08-03
- 决策人：VoiceLife 团队
- 关联：Issue #91、MS3

## 决策

板卡、语音、存储和 IM 实现使用同一 Profile 包络选择，但各自保留独立 Port 和工厂。Adapter 通过稳定 `driver` 名称注册并声明能力；Runtime 在启动时核对必需能力和配置引用。

实现状态：Schema、Profile 校验和按 Profile 构建已经完成；Runtime 工厂注册、能力核对与凭据引用解析尚未实现。ADR 记录的是目标约束，不代表这些运行时能力已经交付。

## 原因

“支持微信、飞书等平台”不应演变为核心里的平台条件分支。只按平台名称切换也不够，因为同一平台的不同通道可能分别支持文本、卡片、动作或回执。能力声明能让 Use Case 明确要求和降级路径，同时让配置成为迁移入口。

## 规则

- Profile 只存选择和非敏感配置，凭据只用引用。
- ESP32 使用编译期注册，不使用动态库或运行时脚本插件。
- Audio、Speech、Storage、IM 不共享万能插件基类。
- Adapter 缺少必需能力时启动失败；可选能力缺失时按已记录策略降级。
- 每类 Adapter 必须通过同一套 Port 契约测试。

## 后果

新增飞书或替换 XRobot 时，核心模型和 Use Case 不需要变化；配置、能力矩阵和迁移步骤可以直接 Review。代价是需要治理 capability 命名、Schema 版本和 driver 兼容别名。
