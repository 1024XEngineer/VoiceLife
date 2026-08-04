# SQLite 存储子架构

## 结论

SQLite 只服务控制面，音频任务不直接持有连接。`voicelife_storage_sqlite` 统一数据库生命周期、迁移、PRAGMA、单写者队列、错误映射和指标；日程、定时任务等领域仍保留自己的业务 Store Port，在各自 Adapter 内把业务操作映射成命名语句。

## 统一协议

公共协议见 [`storage_protocol.h`](../../components/voicelife_storage_sqlite/include/voicelife/storage_sqlite/storage_protocol.h)：

- `StorageRequestContext`：`request_id` 和业务超时，前者用于幂等回读与重放识别；
- `StorageStatement`：稳定的命名语句和结构化参数，不接受原始 SQL、表名或 SQLite 句柄；
- `StorageReadRequest` / `StorageReadResult`：查询只返回结构化行和值以及快照 revision；
- `StorageWriteRequest` / `StorageWriteReceipt`：一组命名语句作为一笔事务提交，回执包含事务 ID、影响行数、提交/重放标记和耗时；
- `StorageHealth`：Schema revision、剩余容量、提交计数和最大提交耗时，供 Runtime/诊断使用。

这套协议统一的是“如何提交和读回控制面事实”，不是一个泛型 `Repository<T>`。`calendar.create`、`timing.claim_due` 等命名语句由对应领域 Adapter 维护，SQLite 底座不理解日程业务，也不开放任意 SQL 执行器。

## 一次写入的边界

```text
Voice/MCP/Application
        -> ScheduleStorePort / TimingStorePort
        -> 领域 SQLite Adapter（命名语句、行映射、幂等回读）
        -> StorageTransactionPort
        -> 单连接、单写者、有界队列、显式事务
        -> FATFS + Wear Levelling + SQLite VFS
```

创建日程的跨表原子性仍由 Application 的粗粒度用例 Port 保证：Schedule、TimingTask、request_dedup 和 Outbox 在同一个 `StorageWriteRequest` 中提交。音频实时路径只发布业务事件或等待业务结果，绝不在 I2S/AFE/Provider worker 中执行 `Commit`。

## 实板约束

当前唯一通过资格测试的基线是 SQLite 3.53.4、FATFS/Wear Levelling、4 KiB 扇区、`journal_mode=DELETE`、`synchronous=EXTRA`、`psow=0`、单连接/单写者。四轮实验的提交中位数约 1.16 秒，已经超过 20/60 ms 音频帧预算；事务必须离开实时任务。

`StorageTransactionPort` 目前是协议和 TDD 骨架，不代表 SQLite Adapter 已接入 Runtime。正式 Adapter 还要完成启动自检、迁移、容量上限、重启恢复、外部 EN/断电差异和 `format_if_mount_failed=false` 现场保护。
