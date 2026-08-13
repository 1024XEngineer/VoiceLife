#include "voicelife/storage_sqlite/voicelife_schema.h"

#include <iterator>

#include "schema/migrations/v001_create_schedule.h"

namespace voicelife::storage_sqlite {
namespace {

/** @brief VoiceLife 数据库从版本零开始按顺序执行的正式迁移清单。 */
constexpr SqliteMigration kMigrations[] = {
    {.version = 1, .apply = &schema::migrations::ApplyV001CreateSchedule},
};

}  // namespace

Status VoiceLifeSchema::Initialize(SqliteDatabase& database) {
    return SqliteSchema::Initialize(database, kCurrentVersion, kMigrations, std::size(kMigrations));
}

}  // namespace voicelife::storage_sqlite
