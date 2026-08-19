#include "voicelife/storage_sqlite/voicelife_schema.h"

#include <iterator>

#include "schema/migrations/v001_create_schedule.h"
#include "schema/migrations/v002_create_schedule_operation.h"
#include "schema/migrations/v003_create_schedule_rule.h"
#include "schema/migrations/v004_create_operation_record.h"
#include "schema/migrations/v005_add_schedule_reminder_task_id.h"
#include "schema/migrations/v006_add_schedule_snooze_state.h"

namespace voicelife::storage_sqlite {
namespace {

/** @brief VoiceLife 数据库从版本零开始按顺序执行的正式迁移清单。 */
constexpr SqliteMigration kMigrations[] = {
    {.version = 1, .apply = &schema::migrations::ApplyV001CreateSchedule},
    {.version = 2, .apply = &schema::migrations::ApplyV002CreateScheduleOperation},
    {.version = 3, .apply = &schema::migrations::ApplyV003CreateScheduleRule},
    {.version = 4, .apply = &schema::migrations::ApplyV004CreateOperationRecord},
    {.version = 5, .apply = &schema::migrations::ApplyV005AddScheduleReminderTaskId},
    {.version = 6, .apply = &schema::migrations::ApplyV006AddScheduleSnoozeState},
};

}  // namespace

Status VoiceLifeSchema::Initialize(SqliteDatabase& database) {
    return SqliteSchema::Initialize(database, kCurrentVersion, kMigrations, std::size(kMigrations));
}

}  // namespace voicelife::storage_sqlite
