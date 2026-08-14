#include "voicelife/im/im_binding_use_case.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace voicelife::im {
namespace {

/// 有效期边界；MCP Schema 已声明 1～10，此处防止绕过 Schema 的直接调用方越界。
constexpr int kMinimumExpiryMinutes = 1;
constexpr int kMaximumExpiryMinutes = 10;

BindingState Map(PairingFlowStatus status) {
    switch (status) {
        case PairingFlowStatus::kIdle:
            return BindingState::kIdle;
        case PairingFlowStatus::kPending:
            return BindingState::kPending;
        case PairingFlowStatus::kWaiting:
            return BindingState::kWaiting;
        case PairingFlowStatus::kRetrying:
            return BindingState::kRetrying;
        case PairingFlowStatus::kAlreadyActive:
            return BindingState::kAlreadyActive;
        case PairingFlowStatus::kConfirmed:
            return BindingState::kConfirmed;
        case PairingFlowStatus::kExpired:
            return BindingState::kExpired;
        case PairingFlowStatus::kCancelled:
            return BindingState::kCancelled;
        case PairingFlowStatus::kNotFound:
            return BindingState::kNotFound;
        case PairingFlowStatus::kTimedOut:
            return BindingState::kTimedOut;
        case PairingFlowStatus::kCredentialRejected:
            return BindingState::kCredentialRejected;
        case PairingFlowStatus::kFailed:
            return BindingState::kFailed;
    }
    return BindingState::kFailed;
}

BindingResult Convert(const PairingFlowResult& result) {
    return {.state = Map(result.status),
            .display_code = result.display_code,
            .expires_at = result.expires_at,
            .message = result.message};
}

}  // namespace

BindingUseCase::BindingUseCase(ImPairingPort& client, ImPairingClock& clock) : client_(&client), clock_(&clock) {}

void BindingUseCase::Bind(ImPairingPort& client, ImPairingClock& clock, std::optional<std::string> user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    client_ = &client;
    clock_ = &clock;
    user_id_ = std::move(user_id);
    controller_.reset();
    state_ = BindingState::kIdle;
}

void BindingUseCase::set_user_id(std::optional<std::string> user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_id_ = std::move(user_id);
}

BindingResult BindingUseCase::Start(int expires_in_minutes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ == nullptr || clock_ == nullptr) {
        state_ = BindingState::kUnavailable;
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = "IM Runtime 尚未 ready"};
    }
    if (expires_in_minutes < kMinimumExpiryMinutes || expires_in_minutes > kMaximumExpiryMinutes) {
        // 参数错误不是绑定状态迁移：不改写 state_，直接返回可播报失败。
        return {.state = BindingState::kFailed,
                .display_code = {},
                .expires_at = {},
                .message = "绑定有效期必须为 1~10 分钟"};
    }
    if (controller_ != nullptr && controller_->active()) {
        state_ = BindingState::kAlreadyActive;
        // 必须携带当前六位码与到期时间：否则“请使用当前绑定码”对用户不可执行。
        return {.state = state_,
                .display_code = controller_->display_code(),
                .expires_at = controller_->expires_at(),
                .message = "已有绑定会话正在进行，请使用当前绑定码"};
    }
    if (!user_id_.has_value() || user_id_->empty()) {
        state_ = BindingState::kUnavailable;
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = "IM 用户引用未配置"};
    }

    controller_ = std::make_unique<PairingSessionController>(*client_, *clock_);
    BindingResult result = Convert(controller_->Begin({.user_id = user_id_, .expires_in_minutes = expires_in_minutes}));
    state_ = result.state;
    return result;
}

BindingResult BindingUseCase::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (controller_ == nullptr || !controller_->active()) {
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = {}};
    }
    BindingResult result = Convert(controller_->Poll());
    state_ = result.state;
    return result;
}

bool BindingUseCase::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return controller_ != nullptr && controller_->active();
}

BindingState BindingUseCase::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

}  // namespace voicelife::im
