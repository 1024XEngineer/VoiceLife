#include "voicelife/contracts/status.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::ErrorCodeName;
using voicelife::test::Check;

int main() {
    Check(std::string_view(ErrorCodeName(ErrorCode::kNone)) == "none", "成功状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(ErrorCode::kInvalidArgument)) == "invalid_argument",
          "无效参数状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(ErrorCode::kNotFound)) == "not_found", "未找到状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(ErrorCode::kAlreadyExists)) == "already_exists", "重复状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(ErrorCode::kConflict)) == "conflict", "冲突状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(ErrorCode::kUnavailable)) == "unavailable", "不可用状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(ErrorCode::kInternal)) == "internal", "内部错误状态应有稳定名称");
    Check(std::string_view(ErrorCodeName(static_cast<ErrorCode>(-1))) == "unknown", "未知状态应回退为 unknown");
    return 0;
}
