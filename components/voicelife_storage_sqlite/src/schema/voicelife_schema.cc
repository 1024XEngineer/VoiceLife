#include "voicelife/storage_sqlite/voicelife_schema.h"

#include <iterator>

#include "schema/migrations/v001_create_schedule.h"
#include "schema/migrations/v002_create_schedule_reminder_delivery.h"
#include "schema/migrations/v003_create_schedule_creation_request.h"

namespace voicelife::storage_sqlite {
namespace {

/** @brief VoiceLife 数据库从版本零开始按顺序执行的正式迁移清单。 */
constexpr SqliteMigration kMigrations[] = {
    {.version = 1, .apply = &schema::migrations::ApplyV001CreateSchedule},
    {.version = 2, .apply = &schema::migrations::ApplyV002CreateScheduleReminderDelivery},
    {.version = 3, .apply = &schema::migrations::ApplyV003CreateScheduleCreationRequest},
};

}  // namespace

Status VoiceLifeSchema::Initialize(SqliteDatabase& database) {
    return SqliteSchema::Initialize(database, kCurrentVersion, kMigrations, std::size(kMigrations));
}

}  // namespace voicelife::storage_sqlite
