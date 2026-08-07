#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "voicelife/contracts/im/reminder_action_command.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/im/im_action_command_stream.h"
#include "voicelife/im/im_action_executor.h"
#include "voicelife/im/im_clock.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_reporting_channel.h"

namespace voicelife::im {

/// 动作通道一次连接的处理结果分类。
enum class ActionRunStatus {
    /// 连接正常结束且全部命令已确认，或仅发生本地丢弃。
    kFinished,
    /// 连接结束但有结果未确认（例如回传网络失败），应重连重放。
    kDisconnected,
    /// 窗口已过期，未建立连接。
    kWindowExpired,
};

/// 强提醒动作流窗口，来自通知受理结果的 actionStream。
struct ActionWindow {
    /// 本次窗口对应的提醒触发标识。
    std::string reminderTriggerId;
    /// 窗口截止时间（ISO-8601 UTC），此后不再处理命令。
    std::string expiresAt;
};

/// 一次动作通道处理的结果。
struct ActionRunResult {
    /// 处理结果分类。
    ActionRunStatus status = ActionRunStatus::kFinished;
    /// 本轮实际执行的命令数（operationId 去重后首次执行）。
    int executed = 0;
    /// 本轮结果已确认（回传成功）的命令数。
    int confirmed = 0;
    /// 本轮因 deviceId/reminderTriggerId 不匹配而被本地丢弃的命令数。
    int dropped = 0;
};

/// 设备侧动作通道：强提醒窗口内建立临时 SSE，执行命令并回传结果。
///
/// 通道不决定是否建立连接——调用方依据通知受理结果的 actionStream 传入
/// 窗口；弱提醒（无窗口）由调用方跳过 Run。通道在窗口过期时拒绝建立。
/// 相同 operationId 的重复命令（含断线重连后的重放）只执行一次，回传
/// 始终以 operationId 作为幂等键；Last-Event-ID 仅用于流游标，不代替
/// 业务确认。
///
/// 本通道为单线程所有权：Run 必须由同一调用方串行调用，实例不可并发
/// 访问；多个强提醒窗口应由调用方排队调度。
class ImActionChannel {
   public:
    /**
     * @brief 创建动作通道。
     * @param reporting 上报通道，负责动作结果 HTTPS 回传。
     * @param credentials 设备凭据，用于命令归属校验。
     * @param executor 本地动作执行端口。
     * @param clock 当前时间来源，用于窗口与命令有效期判断。
     */
    ImActionChannel(ImReportingChannel& reporting, ImCredentialProvider& credentials, ImActionExecutor& executor,
                    ImClock& clock);
    /**
     * @brief 处理一个动作流窗口。
     * @param stream 本次连接的动作命令流，断线重连由调用方新建实例。
     * @param window 动作窗口；expiresAt 已过时不建立连接。
     * @return 本轮处理结果。
     */
    ActionRunResult Run(ImActionCommandStream& stream, const ActionWindow& window);

   private:
    /// 单条已执行结果缓存：随窗口截止毫秒一起记录，窗口过期后由 Run 清理，
    /// 避免设备长期运行后堆积。
    struct CachedExecution {
        int64_t window_expires_ms;
        contracts::im::ReminderActionResult result;
    };
    /// 处理单条命令：校验归属、有效期并执行去重与回传。
    void HandleCommand(const contracts::im::ReminderActionCommand& command, const ActionWindow& window,
                       ActionRunResult& result, bool& has_unconfirmed);
    /// 依据回传结果推进游标并统计确认数。
    void Settle(const ReportResult& report, const contracts::im::ReminderActionCommand& command,
                const std::string& trigger_id, ActionRunResult& result, bool& has_unconfirmed);

    ImReportingChannel& reporting_;
    ImCredentialProvider& credentials_;
    ImActionExecutor& executor_;
    ImClock& clock_;
    /// 上次已确认的 commandId 游标（按提醒触发分区，避免窗口间串扰），
    /// 作为重连时 Last-Event-ID。
    std::map<std::string, std::string> cursors_;
    /// {提醒触发, operationId} -> 已执行结果缓存，保证同窗重复命令只执行一次；
    /// 缓存随窗口截止过期，避免长期运行后无界增长。
    std::map<std::string, CachedExecution> executed_;
};

/// 从网关通知受理结果响应体中提取动作窗口。
///
/// 强提醒受理结果携带 actionStream 时返回对应窗口，调用方据此建立动作流；
/// 弱提醒（无 actionStream）或响应体无法解析时返回空，调用方不得建流。
std::optional<ActionWindow> ExtractActionWindow(const std::string& submission_body);

}  // namespace voicelife::im
