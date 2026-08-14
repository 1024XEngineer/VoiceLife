#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "voicelife/im/im_pairing_controller.h"

namespace voicelife::im {

/// IM 平台绑定用例对交互层可见的稳定状态。
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
    /** 创建该会话时请求的有效期；仅供本地呈现，绝不传入外部协议。 */
    int expires_in_minutes = 0;
    /** Runtime/配置代次；用于丢弃重绑后迟到的旧会话结果。 */
    uint64_t generation = 0;
    std::string message;
};

/// 平台无关的 IM 平台绑定用例；轮询由外部任务调用 Poll 驱动。
///
/// 线程模型：Runtime 的 IM lifecycle 任务调用 Bind 注入依赖，MCP worker 任务
/// 调用 Start，后台轮询任务调用 Poll；三者可能并发，因此全部公开方法以同一把
/// 互斥锁串行化（内部 HTTPS 调用期间也持锁，调用频率低、可接受）。
class BindingUseCase {
   public:
    /** @brief 创建尚未绑定 IM Runtime 的用例；Start 将返回 unavailable。 */
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

    /**
     * @brief 创建短期绑定会话但不执行轮询。
     * @param expires_in_minutes 有效期，必须为 1～10 分钟；越界直接返回 kFailed，不做静默截断。
     * @return 创建结果；已有 active 会话时返回 kAlreadyActive 并携带当前绑定码与到期时间。
     */
    BindingResult Start(int expires_in_minutes = 10);
    /** @brief 推进一次有限轮询状态机。 @return 最近一次脱敏状态。 */
    BindingResult Poll();

    /** @brief 当前是否持有待确认会话。 @return active 时为 true。 */
    [[nodiscard]] bool active() const;
    /** @brief 返回最近一次观察到的绑定状态。 @return 稳定业务状态。 */
    [[nodiscard]] BindingState state() const;
    /** @brief 返回当前 Runtime/会话代次；重绑后旧结果必须被交互层丢弃。
     * @return 当前单调递增的绑定代次。 */
    [[nodiscard]] uint64_t generation() const;

   private:
    ImPairingPort* client_ = nullptr;
    ImPairingClock* clock_ = nullptr;
    std::optional<std::string> user_id_;
    std::unique_ptr<PairingSessionController> controller_;
    BindingState state_ = BindingState::kIdle;
    int active_expiry_minutes_ = 0;
    uint64_t generation_ = 0;
    mutable std::mutex mutex_;
};

}  // namespace voicelife::im
