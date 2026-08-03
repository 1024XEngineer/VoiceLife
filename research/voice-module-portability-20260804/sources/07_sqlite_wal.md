# Source 07：SQLite WAL

- URL：<https://www.sqlite.org/wal.html>
- 读取日期：2026-08-04
- 类型：SQLite 官方文档

## 原文摘录

> WAL mode is persistent; once set, it stays in effect across multiple database connections and after closing and reopening the database.

> With `synchronous=NORMAL`, the database is probably consistent after a power loss, but the most recent transactions might be rolled back.

> With `synchronous=FULL`, transactions are durable once committed.

## 对本项目的约束

存储 Profile 必须同时记录 journal_mode、synchronous、VFS、文件系统和故障类型。SQLite 读写 Port 只能承诺它实际验证过的组合，不提供一个模糊的“SQLite 已支持”开关。
