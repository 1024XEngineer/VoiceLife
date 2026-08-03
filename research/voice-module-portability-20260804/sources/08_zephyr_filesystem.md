# Source 08：Zephyr 文件系统 VFS

- URL：<https://docs.zephyrproject.org/latest/services/file_system/index.html>
- 读取日期：2026-08-04
- 类型：Zephyr 官方文档

## 原文摘录

> VFS allows applications to mount multiple file systems at different mount points.

> VFS decouples the applications from directly accessing an individual file system's specific API.

## 对本项目的判断

跨板卡应复用“应用不直接依赖具体文件系统”的边界，但不能因此假设 FATFS、LittleFS、QSPI、SD 和 SQLite VFS 具有相同掉电语义。公共工具只统一场景协议和结果格式，平台 Adapter 负责实际刷写、断电和存储描述。
