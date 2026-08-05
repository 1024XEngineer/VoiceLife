#pragma once

#include <string>

namespace voicelife {

/// 已序列化的 JSON 文档，具体解析、校验和数据库 JSON 绑定由适配器负责。
struct JsonDocument {
    std::string value;
};

}  // namespace voicelife
