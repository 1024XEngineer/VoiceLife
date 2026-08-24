#pragma once

#include <string>

#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"
#include "voicelife/contracts/im/voice_reminder_action_status.h"
#include "voicelife/im/im_credentials.h"
#include "voicelife/im/im_transport.h"

namespace voicelife::im {

/// 上报提交结果的分类，供调用方决定重试或降级。
enum class ReportStatus {
    /// 服务端已受理，提交成功。
    kSubmitted,
    /// 凭据或 deviceId 校验失败，或服务端以 401/403 拒绝。
    kCredentialRejected,
    /// 网络或服务端暂时失败，可重试；本地事实不受影响。
    kRetryable,
    /// 本地契约校验失败或服务端明确拒绝（4xx/配置错误），不可重试。
    kRejected,
};

/// 一次上报提交的结果。
struct ReportResult {
    /// 提交结果分类。
    ReportStatus status = ReportStatus::kRetryable;
    /// 面向人的结果说明。
    std::string message;
    /// 网关响应体（例如通知受理结果），提交成功且服务端有响应时透传。
    std::string response_body;
};

/// 平台无关的 IM Gateway 上报通道。
///
/// 提交不会修改本地 Schedule、TimerTask 或 ReminderTrigger 事实；
/// 网络失败时返回 kRetryable，由调用方决定重试或降级。
class ImReportingChannel {
   public:
    /**
     * @brief 创建上报通道。
     * @param transport HTTPS 传输实现。
     * @param credentials 设备凭据提供者。
     */
    ImReportingChannel(ImTransport& transport, ImCredentialProvider& credentials);
    /**
     * @brief 提交日程操作回执到 POST /v1/im/schedule-receipts。
     * @param intent 要提交的日程回执。
     * @return 提交结果分类。
     */
    ReportResult SubmitScheduleReceipt(const contracts::im::ScheduleReceiptIntent& intent);
    /**
     * @brief 提交完整日程查询结果到 POST /v1/im/schedule-query-results。
     * @param intent 要提交的日程查询结果。
     * @return 提交结果分类。
     */
    ReportResult SubmitScheduleQueryResult(const contracts::im::ScheduleQueryResultIntent& intent);
    /**
     * @brief 提交提醒通知意图到 POST /v1/im/notifications。
     * @param intent 要提交的提醒通知。
     * @return 提交结果分类。
     */
    ReportResult SubmitNotification(const contracts::im::NotificationIntent& intent);
    /**
     * @brief 回传提醒动作执行结果到 POST /v1/devices/{deviceId}/reminder-actions/{commandId}/result。
     * @param result 执行结果，operationId 作为回传幂等键。
     * @param device_id 命令归属的设备标识。
     * @param command_id 命令标识。
     * @return 提交结果分类。
     */
    ReportResult SubmitReminderActionResult(const contracts::im::ReminderActionResult& result,
                                            const std::string& device_id, const std::string& command_id);
    /** @brief 提交设备语音直接消费后的提醒状态事实。 */
    ReportResult SubmitVoiceReminderActionStatus(const contracts::im::VoiceReminderActionStatus& status);

   private:
    /// 统一提交入口：装配请求头并映射传输结果。
    ReportResult Submit(const std::string& path, const std::string& idempotency_key,
                        const std::string& intent_device_id, const std::string& body);

    ImTransport& transport_;
    ImCredentialProvider& credentials_;
};

}  // namespace voicelife::im
