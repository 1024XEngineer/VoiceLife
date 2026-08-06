# 设备契约版本（schemaVersion）演进规则

本规范约束 `contracts/im-gateway/v1` 下跨端契约（C++ `voicelife_contracts` 与 TypeScript `services/im-gateway` 共用）的版本管理，防止双端契约漂移。

## 1. 单一事实源

- 共享 JSON fixture 位于 `contracts/im-gateway/v1/fixtures`，是跨端 wire contract 的单一事实源。
- C++ 版本常量 `kDeviceContractVersion`（`components/voicelife_contracts/include/voicelife/contracts/im/im_contracts.h`）必须与 TypeScript `DEVICE_CONTRACT_VERSION`（`services/im-gateway/src/contracts/device-gateway.ts`）一致。
- 当前版本：`1`。

## 2. 变更必须双端同步

任何字段、枚举或语义变化必须同时修改：

1. C++ 契约结构与解析（`components/voicelife_contracts/include/voicelife/contracts/im/`）；
2. TypeScript 契约类型与解析（`services/im-gateway/src/contracts/`）；
3. 共享 fixture 及双端测试。

只改一端即视为契约漂移，`scripts/check_contract_dual_end.py` 会在提交前门禁与 CI 中阻止合并。

## 3. 兼容窗口与迁移

- 向后兼容的变更（新增可选字段、扩展枚举）可保持同一 `schemaVersion`，但两端必须同步接收。
- 不兼容变更（删除字段、修改语义、收紧枚举）需要：
  1. 提升 `schemaVersion` 并在两端同步；
  2. 保留旧版本的短期解析入口或兼容窗口；
  3. 单独提交迁移 Issue，说明弃用时间线。
- 阶段性 PR 只写 `Refs #<issue>`；只有契约迁移全部验收完成后才写 `Closes #<issue>`。

## 4. CI 双向把关

- `scripts/check_contract_dual_end.py` 强制双端版本常量一致，且所有**有效** fixture 必须携带该版本（`*-invalid-*` fixture 故意偏离，用于拒绝语义测试，跳过）。
- C++ 主机测试与 TypeScript 测试消费同一批 fixture；任一 fixture 变化会同时破坏两端测试，确保双端同步更新。

## 5. 示例

- 新增 `NotificationContent.body`（可选字段）：双端结构加字段，解析端加可选处理，fixture 可补 body；版本不变。
- 新增 `kind` 取值：双端枚举同步扩展；若旧设备无法处理新取值，则升版本并走迁移窗口。
