# SQLite 存储子架构

## 当前结构

当前先跑通日程创建和查询的最小纵向链路：

```text
ScheduleService
        -> ScheduleRepository
        -> SqliteScheduleRepository
        -> SqliteDatabase / SqliteStatement
        -> sqlite3
```

- `ScheduleService` 只依赖 `ScheduleRepository`，不包含 SQL 和 SQLite 类型；
- `SqliteDatabase` 管理数据库连接、基础 PRAGMA、事务入口和关闭顺序；
- `SqliteStatement` 封装预编译语句、参数绑定、执行和列读取；
- `SqliteScheduleRepository` 只编排 SQL、Statement 和日程行映射，不直接调用 SQLite C API；
- `src/sql/schedule_sql.cc` 集中保存建表、写入和查询 SQL；
- `src/mapping/schedule_row_mapper.cc` 集中处理 `Schedule` 与 SQLite 行之间的转换。

## 当前范围

本阶段只实现三条 SQL：建表、插入日程、查询日程。`ScheduleService::create_schedule` 和
`ScheduleService::query_schedule` 在注入 SQLite Repository 后使用真实数据库；未注入时继续使用现有模拟数据，
避免在搭建存储骨架时重写全部日程测试。

修改、取消、操作记录和撤销仍保持原实现，后续按同一 Repository 边界逐项接入，不在本阶段提前完成全部日程功能。

主机集成测试会创建真实 `.db` 文件，完成写入和查询后关闭连接，再次打开同一文件验证数据仍然存在。
ESP32 正式 Runtime 的 FATFS/Wear Levelling 挂载和组装仍需单独接入，不能用主机测试替代实板验证。

## 实板约束

当前通过资格测试的组合仍是 SQLite 3.53.4、FATFS/Wear Levelling、4 KiB 扇区、
`journal_mode=DELETE`、`synchronous=EXTRA`、`psow=0`、单连接/单写者。SQLite 不得进入音频实时任务；
设备端写入需要通过控制面任务执行。
