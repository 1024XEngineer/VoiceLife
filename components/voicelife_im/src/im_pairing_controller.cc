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

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
    if (right > std::numeric_limits<uint64_t>::max() - left) return std::numeric_limits<uint64_t>::max();
    return left + right;
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
    const unsigned day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

std::optional<uint64_t> ParseIsoMillis(const std::string& input) {
    if (input.size() < 20) return std::nullopt;
    auto digits = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
        if (offset + count > input.size()) return std::nullopt;
        int value = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const char c = input[offset + index];
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    if (!year || !month || !day || !hour || !minute || !second) return std::nullopt;
    std::size_t pos = 19;
    uint64_t fraction_ms = 0;
    if (pos < input.size() && input[pos] == '.') {
        ++pos;
        unsigned digits_seen = 0;
        while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
            if (digits_seen < 3) fraction_ms = fraction_ms * 10 + static_cast<unsigned>(input[pos] - '0');
            ++digits_seen;
            ++pos;
        }
        while (digits_seen < 3) {
            fraction_ms *= 10;
            ++digits_seen;
        }
    }
    int offset_minutes = 0;
    if (pos < input.size() && input[pos] == 'Z') {
        ++pos;
    } else if (pos + 6 == input.size() && (input[pos] == '+' || input[pos] == '-')) {
        const int sign = input[pos] == '+' ? 1 : -1;
        const auto offset_hour = digits(pos + 1, 2);
        const auto offset_minute = digits(pos + 4, 2);
        if (!offset_hour || !offset_minute || input[pos + 3] != ':') return std::nullopt;
        offset_minutes = sign * (*offset_hour * 60 + *offset_minute);
        pos += 6;
    } else {
        return std::nullopt;
    }
    if (pos != input.size()) return std::nullopt;
    const int64_t seconds = DaysFromCivil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) * 86400 +
                            *hour * 3600 + *minute * 60 + *second - offset_minutes * 60;
    if (seconds < 0) return std::nullopt;
    return static_cast<uint64_t>(seconds) * 1000 + fraction_ms;
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
    created_at_ = created.value->session.createdAt;
    user_id_ = created.value->session.userId;
    allowed_platforms_ = created.value->session.allowedPlatforms;
    const uint64_t requested_duration = static_cast<uint64_t>(options.expires_in_minutes) * 60U * 1000U;
    const auto server_created = ParseIsoMillis(created_at_);
    const auto server_expiry = ParseIsoMillis(expires_at_);
    const uint64_t wall_now = clock_.UnixMillis();
    if (!server_created.has_value() || !server_expiry.has_value() || *server_expiry <= *server_created ||
        *server_expiry - *server_created > requested_duration) {
        return Finish(PairingFlowStatus::kFailed, "服务端配对窗口超过请求上限");
    }
    if (*server_expiry <= wall_now) {
        return Finish(PairingFlowStatus::kTimedOut, "服务端配对窗口已过期");
    }
    const uint64_t remaining = std::min(requested_duration, *server_expiry - wall_now);
    deadline_ms_ = SaturatingAdd(now, remaining);
    next_poll_ms_ = SaturatingAdd(now, kPollIntervalMs);
    retry_attempts_ = 0;
    final_poll_attempted_ = false;
    return {
        .status = PairingFlowStatus::kPending, .display_code = display_code_, .expires_at = expires_at_, .message = {}};
}

PairingFlowResult PairingSessionController::Poll() {
    if (!active_) return {.status = PairingFlowStatus::kIdle, .display_code = {}, .expires_at = {}, .message = {}};
    const uint64_t now = clock_.MonotonicMillis();
    const bool final_poll = now >= deadline_ms_;
    if (final_poll && final_poll_attempted_) return Finish(PairingFlowStatus::kTimedOut, "配对已到本地截止时间");
    if (!final_poll && now < next_poll_ms_) {
        return {.status = PairingFlowStatus::kWaiting, .display_code = {}, .expires_at = expires_at_, .message = {}};
    }

    if (final_poll) final_poll_attempted_ = true;
    const PairingQueryResult queried = client_.Query(session_id_);
    if (queried.status == PairingClientStatus::kRetryable) {
        if (final_poll) return Finish(PairingFlowStatus::kTimedOut, queried.message);
        uint64_t delay = kInitialRetryMs;
        for (unsigned index = 0; index < retry_attempts_; ++index) delay = std::min(delay * 2, kMaximumRetryMs);
        if (retry_attempts_ < 31) ++retry_attempts_;
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
    if (queried.value->expiresAt != expires_at_ || queried.value->createdAt != created_at_ ||
        queried.value->userId != user_id_ || queried.value->allowedPlatforms != allowed_platforms_) {
        return Finish(PairingFlowStatus::kFailed, "配对查询响应改变了会话身份或窗口");
    }

    retry_attempts_ = 0;
    if (queried.value->status == "confirmed") return Finish(PairingFlowStatus::kConfirmed);
    if (queried.value->status == "expired") return Finish(PairingFlowStatus::kExpired);
    if (queried.value->status == "cancelled") return Finish(PairingFlowStatus::kCancelled);
    if (final_poll) return Finish(PairingFlowStatus::kTimedOut, "配对已到本地截止时间");
    next_poll_ms_ = std::min(SaturatingAdd(now, kPollIntervalMs), deadline_ms_);
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
