#include "voicelife/im/im_retry_policy.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace voicelife::im {
namespace {

bool IsRetryable(const ImHttpResponse& response) {
    if (response.status == ImTransportStatus::kNetworkFailure) return true;
    if (response.status != ImTransportStatus::kHttpError) return false;
    return response.status_code == 408 || response.status_code == 429 || response.status_code >= 500;
}

}  // namespace

ImRetryPolicy::ImRetryPolicy(ImRetryOptions options) : options_(options) {
    if (options_.initial_delay_ms == 0) options_.initial_delay_ms = 1;
    if (options_.maximum_delay_ms < options_.initial_delay_ms) {
        options_.maximum_delay_ms = options_.initial_delay_ms;
    }
}

std::optional<uint32_t> ImRetryPolicy::NextDelay(const ImHttpResponse& response) {
    if (!IsRetryable(response) || attempts_ >= options_.maximum_attempts) return std::nullopt;

    uint64_t delay = options_.initial_delay_ms;
    for (std::size_t index = 0; index < attempts_ && delay < options_.maximum_delay_ms; ++index) {
        delay = std::min<uint64_t>(delay * 2, options_.maximum_delay_ms);
    }
    ++attempts_;
    return static_cast<uint32_t>(std::min<uint64_t>(delay, std::numeric_limits<uint32_t>::max()));
}

void ImRetryPolicy::Reset() { attempts_ = 0; }

}  // namespace voicelife::im
