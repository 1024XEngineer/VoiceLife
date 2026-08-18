#include "schema/migrations/v002_create_schedule_operation.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

/**
 * @brief 创建日程操作记录表和近期查询索引。
 *
 * 日程快照使用与 schedule 表一致的独立列保存，不使用 JSON 或拼接字符串。
 * previous_id 为空表示操作前不存在日程；撤销操作允许使用这种空快照。
 */
constexpr char kCreateScheduleOperation[] = R"sql(
CREATE TABLE operation_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    type INTEGER NOT NULL CHECK (type IN (1, 2, 3, 4)),
    schedule_id INTEGER NOT NULL,
    schedule_event TEXT NOT NULL CHECK (length(schedule_event) <= 100),
    operated_at INTEGER NOT NULL,
    active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
    previous_id INTEGER,
    previous_event TEXT,
    previous_start_time INTEGER,
    previous_end_time INTEGER,
    previous_location TEXT,
    previous_notes TEXT,
    previous_rule_id INTEGER,
    previous_status INTEGER,
    previous_created_at INTEGER,
    previous_updated_at INTEGER,
    CHECK (previous_id IS NOT NULL OR (
        previous_event IS NULL AND previous_start_time IS NULL AND previous_end_time IS NULL AND
        previous_location IS NULL AND previous_notes IS NULL AND previous_rule_id IS NULL AND
        previous_status IS NULL AND previous_created_at IS NULL AND previous_updated_at IS NULL
    )),
    CHECK (previous_id IS NULL OR previous_event IS NOT NULL),
    CHECK (previous_event IS NULL OR length(previous_event) <= 100),
    CHECK (previous_id IS NULL OR previous_status IN (1, 2, 3)),
    CHECK (previous_id IS NULL OR previous_created_at IS NOT NULL),
    CHECK (previous_id IS NULL OR previous_updated_at IS NOT NULL),
    CHECK (previous_end_time IS NULL OR (previous_start_time IS NOT NULL AND previous_end_time > previous_start_time)),
    CHECK (previous_location IS NULL OR length(previous_location) <= 100),
    CHECK (previous_notes IS NULL OR length(previous_notes) <= 200),
    CHECK ((type = 1 AND previous_id IS NULL) OR (type IN (2, 3) AND previous_id IS NOT NULL) OR type = 4),
    CHECK (previous_id IS NULL OR previous_id = schedule_id)
);
CREATE INDEX operation_record_recent_idx
    ON operation_record (active, operated_at DESC, id DESC);
)sql";

}  // namespace

Status ApplyV002CreateScheduleOperation(SqliteDatabase& database) { return database.Execute(kCreateScheduleOperation); }

}  // namespace voicelife::storage_sqlite::schema::migrations
