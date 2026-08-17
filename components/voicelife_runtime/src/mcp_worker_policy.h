#pragma once

#include <cstdint>

#include "voicelife/im/im_http_policy.h"

namespace voicelife::runtime {

// MCP worker 会同步执行 IM HTTP 请求，因此响应等待时间必须覆盖完整网络超时，
// 并给请求收尾、结果序列化和任务调度留出余量。
inline constexpr uint32_t kMcpResponseGraceMs = 2U * 1000U;
inline constexpr uint32_t kMcpResponseTimeoutMs = im::kImHttpRequestTimeoutMs + kMcpResponseGraceMs;

static_assert(kMcpResponseTimeoutMs > im::kImHttpRequestTimeoutMs,
              "MCP response timeout must exceed the IM HTTP request timeout");

}  // namespace voicelife::runtime
