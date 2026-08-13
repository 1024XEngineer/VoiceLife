#pragma once

#include <memory>
#include <optional>
#include <string>

#include "voicelife/im/im_pairing_controller.h"

namespace voicelife::im {

/// 微信公众号绑定用例对交互层可见的稳定状态。
enum class BindingState {
    kIdle,
    kUnavailable,
    kPending,
    kWaiting,
    kRetrying,
    kAlreadyActive,
    kConfirmed,
    kExpired,
    kCancelled,
    kNotFound,
    kTimedOut,
    kCredentialRejected,
    kFailed,
};

/// 一次开始或轮询操作的脱敏结果。
struct BindingResult {
    BindingState state = BindingState::kIdle;
    std::string display_code;
    std::string expires_at;
    std::string message;
};

/// 平台无关的微信公众号绑定用例；轮询由外部任务调用 Poll 驱动。
class BindingUseCase {
   public:
    /// 创建尚未绑定 IM Runtime 的用例；Start 将返回 unavailable。
    BindingUseCase() = default;
    /** @brief 绑定底层配对端口与时钟。 @param client 配对端口。 @param clock 配对时钟。 */
    BindingUseCase(ImPairingPort& client, ImPairingClock& clock);

    /**
     * @brief 重新绑定 Runtime 依赖并清理旧 active session。
     * @param client Runtime 持有的配对端口。
     * @param clock 可信墙上时钟与单调时钟。
     * @param user_id 已配置的非 Secret 用户引用。
     */
    void Bind(ImPairingPort& client, ImPairingClock& clock, std::optional<std::string> user_id);
    /** @brief 替换用户引用。 @param user_id 已配置的非 Secret 用户引用。 */
    void set_user_id(std::optional<std::string> user_id);

    /** @brief 创建短期绑定会话但不执行轮询。 @param expires_in_minutes 有效期（1～10 分钟）。 */
    BindingResult Start(int expires_in_minutes = 10);
    /** @brief 推进一次有限轮询状态机。 @return 最近一次脱敏状态。 */
    BindingResult Poll();

    /** @brief 当前是否持有待确认会话。 @return active 时为 true。 */
    [[nodiscard]] bool active() const;
    /** @brief 返回最近一次观察到的绑定状态。 @return 稳定业务状态。 */
    [[nodiscard]] BindingState state() const { return state_; }

   private:
    ImPairingPort* client_ = nullptr;
    ImPairingClock* clock_ = nullptr;
    std::optional<std::string> user_id_;
    std::unique_ptr<PairingSessionController> controller_;
    BindingState state_ = BindingState::kIdle;
};

}  // namespace voicelife::im
