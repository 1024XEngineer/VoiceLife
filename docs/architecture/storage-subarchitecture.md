# SQLite 存储子架构

## 分层与组件边界

设备端持久化基础设施按资源生命周期分为两层：

```text
Runtime StorageBootstrap
        -> voicelife_storage_sqlite
           -> sqlite3 / VFS 路径
        -> voicelife_storage_fatfs
           -> FATFS / Wear Levelling / Flash 分区
```

- `voicelife_storage_fatfs` 管理 Flash 数据分区、Wear Levelling、FATFS 挂载和卸载；
- `voicelife_storage_sqlite` 管理连接、语句、Schema 版本、迁移、完整性检查和业务 Repository；
- Runtime 只按顺序组装资源，不包含业务判断；启动顺序是挂载数据卷、打开 SQLite、初始化 Schema、执行 `quick_check`，停止时逆序释放；
- SQLite 通过 VFS 路径访问文件，不依赖 FATFS 组件，也不接触 Wear Levelling 句柄。

`voicelife_storage_sqlite` 保持为一个组件。业务增多时，在组件内部按 `schema`、`sql`、`mapping`、`repository`
等职责组织代码，不为每个业务创建一个 ESP-IDF 存储组件。只有某类存储实现出现独立依赖、生命周期或发布边界时，
才评估拆成新组件。

## 当前范围

本阶段 Runtime 不实例化日程 Repository 或 `ScheduleService`。当前目标 Schema 版本为 1，首次启动会通过正式迁移创建
`schedule` 日程实例表；`rule_id` 只记录来源规则标识，按产品约定不建立数据库外键。Runtime 日志读取并报告
数据库实际 `user_version`，不硬编码版本。

现有日程 SQLite 实现仍留在同一组件内：

```text
ScheduleService
        -> ScheduleRepository
        -> SqliteScheduleRepository
        -> SqliteDatabase / SqliteStatement
        -> sqlite3
```

- `ScheduleService` 只依赖 `ScheduleRepository`，不包含 SQL 和 SQLite 类型；
- `src/sql/schedule_sql.cc` 集中保存日程 SQL；
- `src/mapping/schedule_row_mapper.cc` 集中处理 `Schedule` 与 SQLite 行之间的转换；
- 日程修改、取消、操作记录和撤销不在本阶段扩展。

## 分区策略

正式固件保留当前 `voicelife` 分区声明，通过非空 label 定位分区，并在挂载前严格校验分区类型、地址和容量。
生产挂载固定使用 `format_if_mount_failed=false`：空白、损坏或仍为其他文件系统格式的分区会明确失败并保留现场，
不会在启动过程中静默格式化。新设备的首次 FATFS 初始化属于工厂或维护流程，不属于 Runtime 启动职责。

当前正式 Storage Profile 固定校验根分区表的 `0x7e0000` 起始地址和 `0x200000` 容量。板级探针使用独立测试分区表，
同名分区位于 `0xe00000`；探针固件和正式固件不能交叉作为对方的刷写或挂载依据。

## 实板约束

当前通过资格测试的组合仍是 SQLite 3.53.4、FATFS/Wear Levelling、4 KiB 扇区、
`journal_mode=DELETE`、`synchronous=EXTRA`、`psow=0`、单连接/单写者。SQLite 不得进入音频实时任务；
设备端写入需要通过控制面任务执行。主机测试和 ESP 组件编译不能替代挂载、重启重开和失败保留现场的实板验证。
