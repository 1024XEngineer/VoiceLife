#pragma once

#include <cstdint>
#include <string>

#include "voicelife/im/im_pairing_client.h"

namespace voicelife::im {

/// 配对轮询用可信 UTC 计算服务端剩余窗口，再用单调时钟执行截止。
class ImPairingClock {
   public:
    /** @brief 允许通过接口指针释放单调时钟实现。 */
    virtual ~ImPairingClock() = default;
    /** @brief 返回自启动以来单调递增的毫秒数。 @return 单调毫秒数。 */
    virtual uint64_t MonotonicMillis() const = 0;
    /** @brief 返回 UTC Unix epoch 毫秒数。 @return 已由 Runtime 校准的墙上时钟。 */
    virtual uint64_t UnixMillis() const = 0;
};

/// 配对流程对交互层可见的状态。
enum class PairingFlowStatus {
    kIdle,
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
struct PairingFlowResult {
    PairingFlowStatus status = PairingFlowStatus::kIdle;
    std::string display_code;
    std::string expires_at;
    std::string message;
};

/// 同一 Runtime 最多管理一个 active session 的有限状态轮询器。
class PairingSessionController {
   public:
    /** @brief 绑定配对端口和单调时钟。 @param client 配对端口。 @param clock 单调时钟。 */
    PairingSessionController(ImPairingPort& client, ImPairingClock& clock);
    /** @brief 显式创建会话。 @param options 创建选项。 @return 脱敏流程结果。 */
    PairingFlowResult Begin(const PairingCreateOptions& options);
    /** @brief 到轮询时间时查询一次，否则返回 waiting。 @return 脱敏流程结果。 */
    PairingFlowResult Poll();
    /** @brief 当前是否持有待确认会话。 @return active 时为 true。 */
    [[nodiscard]] bool active() const { return active_; }

   private:
    PairingFlowResult Finish(PairingFlowStatus status, std::string message = {});

    ImPairingPort& client_;
    ImPairingClock& clock_;
    bool active_ = false;
    std::string session_id_;
    std::string display_code_;
    std::string expires_at_;
    std::string created_at_;
    std::optional<std::string> user_id_;
    std::optional<std::vector<std::string>> allowed_platforms_;
    uint64_t deadline_ms_ = 0;
    uint64_t next_poll_ms_ = 0;
    unsigned retry_attempts_ = 0;
    bool final_poll_attempted_ = false;
};

}  // namespace voicelife::im
