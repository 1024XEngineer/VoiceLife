#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/storage_sqlite/sqlite_database.h"
#include "voicelife/storage_sqlite/sqlite_schema.h"

namespace voicelife::storage_sqlite {

/**
 * @brief 管理 VoiceLife 产品数据库的目标版本和正式迁移清单。
 *
 * 通用迁移机制由 SqliteSchema 提供；该类只声明当前固件实际需要的业务表结构。
 */
class VoiceLifeSchema final {
   public:
    /** @brief 当前固件支持的 VoiceLife 数据库 Schema 版本。 */
    static constexpr SchemaVersion kCurrentVersion = 8;

    /**
     * @brief 将已打开的数据库升级到当前 VoiceLife Schema 并执行完整性检查。
     * @param database 已打开的 SQLite 数据库连接。
     * @return 迁移和完整性检查均成功时返回成功状态。
     */
    static Status Initialize(SqliteDatabase& database);

   private:
    /** @brief 该类型只提供静态产品 Schema 操作，禁止实例化。 */
    VoiceLifeSchema() = delete;
};

}  // namespace voicelife::storage_sqlite
