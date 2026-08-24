#include "schema/migrations/v008_add_action_report_state.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

constexpr char kAddActionReportState[] = R"sql(
ALTER TABLE schedule_reminder_task
    ADD COLUMN action_reported INTEGER NOT NULL DEFAULT 0;
)sql";

}  // namespace

Status ApplyV008AddActionReportState(SqliteDatabase& database) { return database.Execute(kAddActionReportState); }

}  // namespace voicelife::storage_sqlite::schema::migrations
