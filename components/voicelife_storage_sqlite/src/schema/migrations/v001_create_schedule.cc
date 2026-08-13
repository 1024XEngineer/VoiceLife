#include "schema/migrations/v001_create_schedule.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

/**
 * @brief 创建一次性日程和周期规则生成的具体日程实例。
 *
 * SQLite 使用 UTC Unix 秒整数持久化时间。rule_id 仅记录来源规则标识，按产品约定不建立外键。
 */
constexpr char kCreateSchedule[] = R"sql(
CREATE TABLE schedule (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_id INTEGER,
    event TEXT NOT NULL CHECK (length(event) <= 100),
    start_time INTEGER,
    end_time INTEGER,
    location TEXT CHECK (location IS NULL OR length(location) <= 100),
    notes TEXT CHECK (notes IS NULL OR length(notes) <= 200),
    status INTEGER NOT NULL DEFAULT 1 CHECK (status IN (1, 2, 3)),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    CHECK (end_time IS NULL OR (start_time IS NOT NULL AND end_time > start_time))
);
)sql";

}  // namespace

Status ApplyV001CreateSchedule(SqliteDatabase& database) { return database.Execute(kCreateSchedule); }

}  // namespace voicelife::storage_sqlite::schema::migrations
