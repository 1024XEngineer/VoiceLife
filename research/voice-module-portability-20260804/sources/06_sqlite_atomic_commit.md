# Source 06：SQLite Atomic Commit

- URL：<https://www.sqlite.org/atomiccommit.html>
- 读取日期：2026-08-04
- 类型：SQLite 官方文档

## 原文摘录

> SQLite uses a rollback journal to make transactions atomic.

> The database file is not updated until the transaction has been committed.

> The sync operation must ensure that all writes have reached stable storage before it returns.

## 对本项目的约束

SQLite 的 `commit` 语义依赖 VFS 的 sync、锁、扇区和介质实现；不能用“文件 API 能写”替代实板掉电证据。音频任务不得直接持有 SQLite 连接，业务事件应进入控制面存储队列。
