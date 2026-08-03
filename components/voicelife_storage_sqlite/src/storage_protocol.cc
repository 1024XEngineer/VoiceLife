#include "voicelife/storage_sqlite/storage_protocol.h"

#include <cctype>

namespace voicelife::storage_sqlite {
namespace {

bool IsStatementName(std::string_view name) {
    if (name.empty() || !std::islower(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    for (const unsigned char character : name) {
        if (!(std::islower(character) || std::isdigit(character) || character == '_' ||
              character == '.' || character == '-')) {
            return false;
        }
    }
    return true;
}

}  // namespace

Status StorageStatement::Validate() const {
    if (!IsStatementName(name)) {
        return Status::Error(ErrorCode::kInvalidArgument, "存储命名语句标识无效");
    }
    return Status::Ok();
}

Status StorageRequestContext::Validate() const {
    if (request_id.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "存储请求缺少 request_id");
    }
    if (deadline_ms == 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "存储请求 deadline_ms 必须大于零");
    }
    return Status::Ok();
}

Status StorageReadRequest::Validate() const {
    Status status = context.Validate();
    if (!status.ok()) {
        return status;
    }
    return query.Validate();
}

Status StorageWriteRequest::Validate() const {
    Status status = context.Validate();
    if (!status.ok()) {
        return status;
    }
    if (statements.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "存储写事务不能为空");
    }
    for (const StorageStatement& statement : statements) {
        status = statement.Validate();
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

}  // namespace voicelife::storage_sqlite
