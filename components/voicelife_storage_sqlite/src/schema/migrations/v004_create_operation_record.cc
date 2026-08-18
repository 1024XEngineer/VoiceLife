#include "schema/migrations/v004_create_operation_record.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

/**
 * @brief 重建操作记录表为纯审计日志结构。
 *
 * 新表只保存实体类型、操作类型、实体标识、展示名称、操作时间和 opaque 的
 * before JSON 快照；不再有 active 软删除与撤销链。旧表数据与结构不兼容，
 * 直接删除后重建（操作日志为短期记录，不迁移历史行）。
 */
constexpr char kRebuildOperationRecord[] = R"sql(
DROP TABLE IF EXISTS operation_record;

CREATE TABLE operation_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_type INTEGER NOT NULL CHECK (entity_type IN (1, 2, 3)),
    type INTEGER NOT NULL CHECK (type IN (1, 2, 3)),
    entity_id INTEGER NOT NULL CHECK (entity_id > 0),
    label TEXT NOT NULL CHECK (length(label) BETWEEN 1 AND 100),
    operated_at INTEGER NOT NULL,
    before TEXT,
    CHECK ((type = 1 AND before IS NULL) OR (type IN (2, 3) AND before IS NOT NULL))
);

CREATE INDEX operation_record_recent_idx ON operation_record (operated_at DESC, id DESC);
)sql";

}  // namespace

Status ApplyV004CreateOperationRecord(SqliteDatabase& database) { return database.Execute(kRebuildOperationRecord); }

}  // namespace voicelife::storage_sqlite::schema::migrations
