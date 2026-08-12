#include "voicelife/im/im_pairing_controller.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace voicelife::im {
namespace {

constexpr uint64_t kPollIntervalMs = 3000;
constexpr uint64_t kInitialRetryMs = 2000;
constexpr uint64_t kMaximumRetryMs = 5000;
constexpr unsigned kMaximumRetryAttempts = 4;

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
    if (right > std::numeric_limits<uint64_t>::max() - left) return std::numeric_limits<uint64_t>::max();
    return left + right;
}

}  // namespace

PairingSessionController::PairingSessionController(ImPairingPort& client, ImPairingClock& clock)
    : client_(client), clock_(clock) {}

PairingFlowResult PairingSessionController::Begin(const PairingCreateOptions& options) {
    if (active_) {
        return {
            .status = PairingFlowStatus::kAlreadyActive, .display_code = {}, .expires_at = expires_at_, .message = {}};
    }
    const PairingCreateResult created = client_.Create(options);
    if (created.status != PairingClientStatus::kSuccess || !created.value.has_value()) {
        if (created.status == PairingClientStatus::kCredentialRejected) {
            return {.status = PairingFlowStatus::kCredentialRejected,
                    .display_code = {},
                    .expires_at = {},
                    .message = created.message};
        }
        return {.status = PairingFlowStatus::kFailed, .display_code = {}, .expires_at = {}, .message = created.message};
    }

    const uint64_t now = clock_.MonotonicMillis();
    active_ = true;
    session_id_ = created.value->session.id;
    display_code_ = created.value->displayCode;
    expires_at_ = created.value->session.expiresAt;
    const uint64_t duration = static_cast<uint64_t>(options.expires_in_minutes) * 60U * 1000U;
    deadline_ms_ = SaturatingAdd(now, duration);
    next_poll_ms_ = SaturatingAdd(now, kPollIntervalMs);
    retry_attempts_ = 0;
    return {
        .status = PairingFlowStatus::kPending, .display_code = display_code_, .expires_at = expires_at_, .message = {}};
}

PairingFlowResult PairingSessionController::Poll() {
    if (!active_) return {.status = PairingFlowStatus::kIdle, .display_code = {}, .expires_at = {}, .message = {}};
    const uint64_t now = clock_.MonotonicMillis();
    if (now >= deadline_ms_) return Finish(PairingFlowStatus::kTimedOut, "配对已到本地截止时间");
    if (now < next_poll_ms_) {
        return {.status = PairingFlowStatus::kWaiting, .display_code = {}, .expires_at = expires_at_, .message = {}};
    }

    const PairingQueryResult queried = client_.Query(session_id_);
    if (queried.status == PairingClientStatus::kRetryable) {
        if (retry_attempts_ >= kMaximumRetryAttempts) return Finish(PairingFlowStatus::kFailed, queried.message);
        uint64_t delay = kInitialRetryMs;
        for (unsigned index = 0; index < retry_attempts_; ++index) delay = std::min(delay * 2, kMaximumRetryMs);
        ++retry_attempts_;
        next_poll_ms_ = std::min(SaturatingAdd(now, delay), deadline_ms_);
        return {.status = PairingFlowStatus::kRetrying,
                .display_code = {},
                .expires_at = expires_at_,
                .message = queried.message};
    }
    if (queried.status == PairingClientStatus::kNotFound) return Finish(PairingFlowStatus::kNotFound, queried.message);
    if (queried.status == PairingClientStatus::kCredentialRejected) {
        return Finish(PairingFlowStatus::kCredentialRejected, queried.message);
    }
    if (queried.status != PairingClientStatus::kSuccess || !queried.value.has_value()) {
        return Finish(PairingFlowStatus::kFailed, queried.message);
    }

    retry_attempts_ = 0;
    if (queried.value->status == "confirmed") return Finish(PairingFlowStatus::kConfirmed);
    if (queried.value->status == "expired") return Finish(PairingFlowStatus::kExpired);
    if (queried.value->status == "cancelled") return Finish(PairingFlowStatus::kCancelled);
    next_poll_ms_ = SaturatingAdd(now, kPollIntervalMs);
    return {.status = PairingFlowStatus::kPending, .display_code = {}, .expires_at = expires_at_, .message = {}};
}

PairingFlowResult PairingSessionController::Finish(PairingFlowStatus status, std::string message) {
    PairingFlowResult result{
        .status = status, .display_code = {}, .expires_at = expires_at_, .message = std::move(message)};
    active_ = false;
    session_id_.clear();
    display_code_.clear();
    retry_attempts_ = 0;
    return result;
}

}  // namespace voicelife::im
