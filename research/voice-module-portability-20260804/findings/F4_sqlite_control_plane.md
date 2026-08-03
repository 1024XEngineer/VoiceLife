# F4：SQLite 统一读写协议属于控制面

## 判断

H4 得到支持。统一读写协议不应是把 `sqlite3*` 暴露给所有模块，而是一个版本化、事务边界明确的 `StorageTransactionPort`：调用方提交业务命令/查询，Adapter 独占连接并返回结构化结果；音频任务只发布异步业务事件。

## 最小协议

```text
Begin(transaction_id)
  -> Execute(command, parameters)
  -> Commit | Rollback
Result { transaction_id, committed, rows_affected, integrity_digest, error }
```

每次实板运行还要记录 `journal_mode`、`synchronous`、SQLite 版本、VFS/文件系统、介质、故障方式和恢复后的 `integrity_check`。不能用一个布尔值表示“SQLite 支持”。

## 与语音的连接

MCP/Calendar Application 在控制面消费 STT/工具事件并写库；实时音频数据面只维护 generation、队列和硬件。提醒播报通过 Provider/Notification Port 触发，不能在音频回调里同步提交事务。
