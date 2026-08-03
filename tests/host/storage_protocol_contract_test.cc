#include "support/test_support.h"
#include "voicelife/storage_sqlite/storage_protocol.h"

using voicelife::ErrorCode;
using voicelife::storage_sqlite::StorageReadRequest;
using voicelife::storage_sqlite::StorageStatement;
using voicelife::storage_sqlite::StorageWriteRequest;
using voicelife::test::Check;

int main() {
    StorageWriteRequest write{
        .context = {.request_id = "req-42", .deadline_ms = 3000},
        .statements = {{.name = "calendar.create", .arguments = {std::int64_t{42}, true}}},
    };
    Check(write.Validate().ok(), "命名语句写事务应通过校验");
    Check(std::get<std::int64_t>(write.statements.front().arguments.front()) == 42,
          "结构化值应保留整数类型");

    StorageReadRequest read{
        .context = {.request_id = "req-43", .deadline_ms = 1000},
        .query = {.name = "calendar.list", .arguments = {std::string{"active"}}},
    };
    Check(read.Validate().ok(), "命名语句读请求应通过校验");

    auto missing_request = write;
    missing_request.context.request_id.clear();
    Check(missing_request.Validate().code == ErrorCode::kInvalidArgument,
          "缺少 request_id 必须拒绝");

    auto empty_write = write;
    empty_write.statements.clear();
    Check(empty_write.Validate().code == ErrorCode::kInvalidArgument,
          "空写事务必须拒绝");

    auto raw_sql = write;
    raw_sql.statements.front().name = "INSERT schedule";
    Check(raw_sql.Validate().code == ErrorCode::kInvalidArgument,
          "原始 SQL 或表名不能进入统一协议");

    StorageStatement invalid_query{.name = "Calendar.List"};
    Check(invalid_query.Validate().code == ErrorCode::kInvalidArgument,
          "命名语句必须使用稳定的小写标识");
    return 0;
}
