#pragma once

#include <cstdint>

namespace voicelife::im {

// 单次 IM 网关请求允许占用的最长时间。调用这类请求的上层等待预算必须更长，
// 否则网络操作可能已经成功，上层却先把结果报告为失败。
inline constexpr uint32_t kImHttpRequestTimeoutMs = 10U * 1000U;

}  // namespace voicelife::im
