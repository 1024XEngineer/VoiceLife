# SQLite amalgamation

该目录保存 VoiceLife 在 ESP-IDF 上使用的 SQLite 组件模板和兼容性补丁。
`sqlite3.c`、`sqlite3.h` 与生成的 `CMakeLists.txt` 不纳入 Git，由根目录脚本按固定摘要生成：

```bash
python3 scripts/prepare_sqlite.py
```

当前固定版本为 SQLite `3.53.4`。脚本会校验下载压缩包、解压后的 amalgamation
文件和 ESP-IDF `unix-none` VFS 补丁；正式固件与板级探针共享同一目录，避免维护两套来源。

SQLite 源码按其发布许可使用；本目录中的 ESP-IDF 补丁和组件模板属于 VoiceLife 工程文件。
