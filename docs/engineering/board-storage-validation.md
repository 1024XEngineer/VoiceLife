# SQLite 实板验证与 Flash 恢复手册

VoiceLife 当前只允许把 `SQLite 3.53.4 + FATFS/Wear Levelling` 作为板上持久化实现基线；`LittleFS + SQLite` 已被实板回滚测试否决。任何替代文件系统或 VFS 都要重新跑同一套故障测试，不能引用上游说明代替本板结果。

本手册用于复现实验并保护板上数据。执行人必须先确认非活动 OTA 槽，始终使用 `115200`，完成备份和回读校验后才能写入；结束时先恢复业务数据，再按备份还原目标 OTA 槽，最后恢复 OTA 元数据。

## 已确认的结论

目标环境：ESP32-S3、ESP-IDF 6.0.2、2 MiB `voicelife` 数据分区。

通过配置：

```text
SQLite               3.53.4
Filesystem           FATFS + Wear Levelling
FATFS/WL sector      4096 bytes
journal_mode         DELETE
synchronous          EXTRA
powersafe overwrite  0
database owner       single connection / single writer
```

四轮独立测试均通过显式回滚、40 次四表原子提交、每次提交后的表/索引一致性、`PRAGMA quick_check`、关闭重开、幂等唯一约束，以及两个外部 EN 复位故障点：

| 轮次 | 平均提交耗时 | 最慢提交 |
| --- | ---: | ---: |
| 1 | 1,110,342 us | 1,183,910 us |
| 2 | 1,147,655 us | 1,228,195 us |
| 3 | 1,177,090 us | 1,263,605 us |
| 4（公开工具回归） | 1,316,958 us | 1,518,898 us |

四轮平均值的中位数是 `1,162,373 us`，观测到的最慢提交是 `1,518,898 us`，最低观测空闲堆为 `211,116 bytes`。这意味着持久化必须离开音频实时路径，由单写者串行执行。

以下 LittleFS 组合均在显式 `ROLLBACK` 后出现表记录残留、索引记录缺失，因此不能进入生产实现：

```text
PERSIST + FULL + psow=1  -> FAIL
PERSIST + FULL + psow=0  -> FAIL
DELETE  + FULL + psow=0  -> FAIL

Explicit rollback integrity: table_rows=1 index_rows=0
PROBE_RESULT: FAIL step=explicit rollback leaked row
```

## 从实板故障中固定下来的规则

1. **串口固定为 `115200`。** 本板在 `460800/921600` 读取大分区时随机断流。慢几分钟比拿到无法证明完整的备份更便宜。
2. **分区表必须从芯片读取并解析。** 不根据 CSV、旧日志或人工抄写的偏移执行擦写；名称、类型、偏移、长度任一不符都停止。
3. **写入前先备份，备份后做摘要校验。** 数据分区、目标 OTA 槽和 `otadata` 都要完整读取并执行 `verify_flash`。AK、SK、业务数据和备份文件都不进入仓库。
4. **只写人工确认的非活动 OTA 槽。** `--confirm-inactive-slot` 是强制确认，不代表工具能替执行人判断当前运行槽；先从设备启动日志确认 `Running partition`。
5. **连续 Flash 操作必须保持 bootloader stub。** 第一步连接后使用 `after=no_reset`，中间步骤使用 `before=no_reset/after=no_reset`，最后一步才硬复位。
6. **不相信擦除或写入命令的退出码。** 每次擦除和写入都要完整 `verify_flash` 或使用等价的芯片侧摘要。曾出现过擦除命令报告成功、数据库旧记录仍在的情况。
7. **恢复顺序固定。** 先写回并校验业务数据，再恢复并校验目标 OTA 槽，最后写回并校验 `otadata`。原槽全为 `0xFF` 时使用整槽擦除，否则写回原镜像。启动指针在最后变更，便于中断后继续诊断。
8. **准确描述故障类型。** 串口控制线触发的是外部 EN 复位，启动原因是 `rst:0x1 (POWERON)`；它比 `esp_restart()` 更接近故障恢复，但不等于切断电源轨。
9. **实验初始化与生产挂载分开。** 探针用 `CONFIG_SQLITE_PROBE_ALLOW_FORMAT=y` 初始化已备份的测试分区；正式 Adapter 必须设置 `format_if_mount_failed=false`，挂载失败时保留现场。实板已经证明，关闭实验初始化会让非 FATFS 测试介质立即拒绝挂载；这正是生产环境应保留的失败。
10. **一轮实验必须从 phase 0 开始。** 探针把固件镜像指纹与 phase 一起持久化，主机只接受同一镜像的 `0 -> 1 -> 2`。旧镜像留下的 phase 不算本轮证据。
11. **工具依赖在写入前检查。** `write-probe` 需要 `IDF_PATH` 下的 `otatool.py`；未加载 ESP-IDF 环境时必须在写 Flash 前退出。

## 准备和构建探针

生成目录中的 SQLite 源码不入库。脚本固定下载地址、归档摘要和补丁后文件摘要，任一不匹配都会失败。

```bash
python3 tests/board/sqlite_storage_probe/prepare_sqlite.py

source /path/to/esp-idf-v6.0.2/export.sh
cd tests/board/sqlite_storage_probe
idf.py build
cd ../../..
```

探针构建必须显示 ESP-IDF `v6.0.2`，生成的应用镜像必须小于目标 OTA 槽。固件默认挂载并允许初始化已备份的 `voicelife` 测试分区；`source export.sh` 还会设置 `IDF_PATH`，不要在另一个未加载环境的 shell 中执行 `write-probe`。

## 运行完整实验

以下示例中的端口是占位符，`ota_1` 也必须由执行人确认确为非活动槽。换板时先读取布局并替换，不能照抄。

```bash
PORT=/dev/cu.usbmodemXXXX
BACKUP=/tmp/voicelife-sqlite-backup

python3 scripts/sqlite_board_probe.py inspect \
  --port "$PORT" \
  --baud 115200 \
  --output /tmp/voicelife-partition-table.bin

python3 scripts/sqlite_board_probe.py backup \
  --port "$PORT" \
  --baud 115200 \
  --directory "$BACKUP" \
  --probe-slot ota_1

python3 scripts/sqlite_board_probe.py write-probe \
  --port "$PORT" \
  --baud 115200 \
  --backup-directory "$BACKUP" \
  --probe-slot ota_1 \
  --confirm-inactive-slot ota_1 \
  --binary tests/board/sqlite_storage_probe/build/voicelife_sqlite_storage_probe.bin \
  --yes

python3 scripts/sqlite_board_probe.py monitor \
  --port "$PORT" \
  --baud 115200 \
  --timeout 300
```

只有同时出现以下内容才算一轮通过：

```text
PROBE_PHASE: phase=0 image=<same-image-id>
HOST_RESET_POINT: OPEN_TRANSACTION
HOST_ACTION: EN_RESET point=OPEN_TRANSACTION
PROBE_PHASE: phase=1 image=<same-image-id>
HOST_RESET_POINT: AFTER_COMMIT
HOST_ACTION: EN_RESET point=AFTER_COMMIT
PROBE_PHASE: phase=2 image=<same-image-id>
PROBE_RESULT: PASS ... reset=host-en ...
HOST_RESULT: PASS resets=OPEN_TRANSACTION,AFTER_COMMIT
```

`PROBE_RESULT: FAIL`、固件 abort、phase 或复位点缺失、镜像指纹变化、顺序错误或超时都算失败。不得只截取最终 `PASS` 行。

## 恢复板子

无论实验成功还是失败，都要恢复。工具会重新读取分区表、核对 manifest 与每个镜像的摘要和尺寸，并按“数据 -> 原目标槽 -> OTA 元数据”的顺序操作。

```bash
python3 scripts/sqlite_board_probe.py restore \
  --port "$PORT" \
  --baud 115200 \
  --directory "$BACKUP" \
  --yes
```

最后重新抓取启动日志，至少确认：

- `Project name` 是原业务固件，不是 `voicelife_sqlite_storage_probe`；
- `Running partition` 回到实验前的槽；
- 业务存储加载数量与备份前一致；
- 挂载、完整性检查和主循环没有新增错误。

本次恢复实测确认原固件为 `xiaozhi`、运行槽回到 `ota_0`，业务存储重新加载 `7 events, 8 reminders, 0 notes`。

## 当前仍未通过的验收

四轮 EN 复位只够确定当前技术路线，不构成完整生产认证。合入真实 SQLite Adapter 前仍要补齐：

- 至少 10 轮连续实板回归；
- 可控电源轨断电和棕断测试；
- 2 MiB 容量耗尽、空间恢复和数据库完整性；
- 长期写放大、磨损和寿命预算；
- 正式 schema 迁移失败与损坏数据库受限启动；
- 与音频实时任务并行时的队列、延迟和内存水位。

## 设计依据

- [SQLite Atomic Commit](https://www.sqlite.org/atomiccommit.html)
- [SQLite VFS](https://www.sqlite.org/vfs.html)
- [SQLite synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous)
- [SQLite powersafe overwrite](https://www.sqlite.org/psow.html)
- [ESP-IDF 6.0.2 Wear Levelling](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/api-reference/storage/wear-levelling.html)
