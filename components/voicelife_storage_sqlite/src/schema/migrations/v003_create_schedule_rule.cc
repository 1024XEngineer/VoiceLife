#include "schema/migrations/v003_create_schedule_rule.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

/**
 * @brief 创建周期规则表、单次例外表及查询索引。
 *
 * 时间存储约定（与 schedule 表一致，均使用 INTEGER）：
 * - `start_date` / `end_date`：自 1970-01-01 起的天数；
 * - `start_time` / `end_time`：当日 0 点起的秒数（0~86399）；
 * - `original_start_time` / `override_*`：UTC Unix 秒；
 * - `created_at` / `updated_at`：UTC Unix 秒。
 *
 * `rule_id`、`schedule_id` 按产品约定不建立数据库外键，由事务层保证引用完整性。
 */
constexpr char kCreateScheduleRule[] = R"sql(
CREATE TABLE schedule_rule (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event TEXT NOT NULL CHECK (length(event) <= 100),
    location TEXT CHECK (location IS NULL OR length(location) <= 100),
    notes TEXT CHECK (notes IS NULL OR length(notes) <= 200),
    freq_type INTEGER NOT NULL CHECK (freq_type IN (1, 2, 3, 4)),
    interval_val INTEGER NOT NULL DEFAULT 1 CHECK (interval_val > 0),
    weekdays_mask INTEGER CHECK (weekdays_mask IS NULL OR (weekdays_mask BETWEEN 1 AND 127)),
    day_of_month INTEGER CHECK (day_of_month IS NULL OR (day_of_month BETWEEN 1 AND 31)),
    month_of_year INTEGER CHECK (month_of_year IS NULL OR (month_of_year BETWEEN 1 AND 12)),
    monthly_mode INTEGER CHECK (monthly_mode IS NULL OR monthly_mode IN (1, 2)),
    start_time INTEGER NOT NULL CHECK (start_time BETWEEN 0 AND 86399),
    end_time INTEGER CHECK (end_time IS NULL OR (end_time BETWEEN 0 AND 86399 AND end_time > start_time)),
    start_date INTEGER NOT NULL,
    end_date INTEGER CHECK (end_date IS NULL OR end_date >= start_date),
    occurrence_count INTEGER CHECK (occurrence_count IS NULL OR occurrence_count > 0),
    status INTEGER NOT NULL DEFAULT 1 CHECK (status IN (1, 2, 3)),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    CHECK (occurrence_count IS NULL OR end_date IS NULL)
);

CREATE TABLE schedule_rule_exception (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_id INTEGER NOT NULL,
    original_start_time INTEGER NOT NULL,
    schedule_id INTEGER,
    type INTEGER NOT NULL CHECK (type IN (1, 2)),
    override_start_time INTEGER,
    override_end_time INTEGER,
    override_event TEXT CHECK (override_event IS NULL OR length(override_event) <= 100),
    override_location TEXT CHECK (override_location IS NULL OR length(override_location) <= 100),
    override_notes TEXT CHECK (override_notes IS NULL OR length(override_notes) <= 200),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (rule_id, original_start_time),
    CHECK (type = 1 OR (
        override_start_time IS NULL AND override_end_time IS NULL AND
        override_event IS NULL AND override_location IS NULL AND override_notes IS NULL
    ))
);

CREATE INDEX schedule_rule_exception_rule_idx ON schedule_rule_exception (rule_id);
CREATE INDEX schedule_rule_exception_schedule_idx ON schedule_rule_exception (schedule_id);
CREATE INDEX schedule_rule_idx ON schedule (rule_id);
CREATE INDEX schedule_start_time_idx ON schedule (start_time);
)sql";

}  // namespace

Status ApplyV003CreateScheduleRule(SqliteDatabase& database) { return database.Execute(kCreateScheduleRule); }

}  // namespace voicelife::storage_sqlite::schema::migrations
