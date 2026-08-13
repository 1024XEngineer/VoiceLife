#include "schema/migrations/v003_create_schedule_creation_request.h"

namespace voicelife::storage_sqlite::schema::migrations {
namespace {

constexpr char kCreateScheduleCreationRequest[] = R"sql(
CREATE TABLE schedule_creation_request (
    request_key TEXT PRIMARY KEY CHECK (length(request_key) BETWEEN 1 AND 128),
    schedule_id INTEGER NOT NULL REFERENCES schedule(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL
);
)sql";

}  // namespace

Status ApplyV003CreateScheduleCreationRequest(SqliteDatabase& database) {
    return database.Execute(kCreateScheduleCreationRequest);
}

}  // namespace voicelife::storage_sqlite::schema::migrations
