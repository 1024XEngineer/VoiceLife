#include "voicelife/im/im_binding_use_case.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace voicelife::im {
namespace {

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
    client_ = &client;
    clock_ = &clock;
    user_id_ = std::move(user_id);
    controller_.reset();
    state_ = BindingState::kIdle;
}

void BindingUseCase::set_user_id(std::optional<std::string> user_id) { user_id_ = std::move(user_id); }

BindingResult BindingUseCase::Start(int expires_in_minutes) {
    if (client_ == nullptr || clock_ == nullptr) {
        state_ = BindingState::kUnavailable;
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = "IM Runtime 尚未 ready"};
    }
    if (controller_ != nullptr && controller_->active()) {
        state_ = BindingState::kAlreadyActive;
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = "已有绑定会话正在进行"};
    }
    if (!user_id_.has_value() || user_id_->empty()) {
        state_ = BindingState::kUnavailable;
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = "IM 用户引用未配置"};
    }

    controller_ = std::make_unique<PairingSessionController>(*client_, *clock_);
    BindingResult result =
        Convert(controller_->Begin({.user_id = user_id_, .expires_in_minutes = std::clamp(expires_in_minutes, 1, 10)}));
    state_ = result.state;
    return result;
}

BindingResult BindingUseCase::Poll() {
    if (controller_ == nullptr || !controller_->active()) {
        return {.state = state_, .display_code = {}, .expires_at = {}, .message = {}};
    }
    BindingResult result = Convert(controller_->Poll());
    state_ = result.state;
    return result;
}

bool BindingUseCase::active() const { return controller_ != nullptr && controller_->active(); }

}  // namespace voicelife::im
