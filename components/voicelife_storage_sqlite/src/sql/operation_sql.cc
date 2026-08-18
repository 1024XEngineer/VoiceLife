#include "sql/operation_sql.h"

#include <string>

namespace voicelife::storage_sqlite::sql {

const char kInsertOperation[] = R"sql(
INSERT INTO operation_record (
    entity_type, type, entity_id, label, operated_at, before
) VALUES (?, ?, ?, ?, ?, ?)
)sql";

const char kOperationColumns[] =
    "id, entity_type, type, entity_id, label, operated_at, before";

std::string BuildOperationWhere() {
    return R"sql(
WHERE (?1 IS NULL OR id = ?1)
  AND (?2 IS NULL OR entity_type = ?2)
  AND (?3 IS NULL OR entity_id = ?3)
  AND (?4 IS NULL OR type = ?4)
  AND (?5 IS NULL OR operated_at >= ?5)
  AND (?6 IS NULL OR operated_at <= ?6)
  AND (?7 IS NULL OR label LIKE '%' || ?7 || '%')
)sql";
}

std::string BuildOperationFindSql(const schedule::QueryOperationCommand& query) {
    (void)query;
    return std::string("SELECT ") + kOperationColumns + "\nFROM operation_record\n" + BuildOperationWhere() +
           "\nORDER BY operated_at DESC, id DESC\nLIMIT ?8 OFFSET ?9";
}

std::string BuildOperationCountSql(const schedule::QueryOperationCommand& query) {
    (void)query;
    return std::string("SELECT COUNT(*)\nFROM operation_record\n") + BuildOperationWhere();
}

}  // namespace voicelife::storage_sqlite::sql
