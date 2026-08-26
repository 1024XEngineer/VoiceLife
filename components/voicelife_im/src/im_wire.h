#pragma once

#include <string>
#include <string_view>

#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"

namespace voicelife::im {

/// 对 URL path/query 中的标识做百分号编码，仅保留 RFC 3986 非保留字符，
/// 防止 deviceId/commandId 等含 / ? & # 时改写请求路径或参数。
std::string EncodePathSegment(std::string_view segment);
/// 把日程操作回执序列化为网关契约 JSON 文本。
std::string SerializeScheduleReceiptIntent(const contracts::im::ScheduleReceiptIntent& intent);
/// 把完整日程查询结果序列化为网关契约 JSON 文本。
std::string SerializeScheduleQueryResultIntent(const contracts::im::ScheduleQueryResultIntent& intent);
/// 把提醒通知意图序列化为网关契约 JSON 文本。
std::string SerializeNotificationIntent(const contracts::im::NotificationIntent& intent);
/// 把提醒动作执行结果序列化为网关契约 JSON 文本。
std::string SerializeReminderActionResult(const contracts::im::ReminderActionResult& result);
/// 把独立设备语音动作事实序列化为网关契约 JSON 文本。
std::string SerializeReminderActionStatusReport(const contracts::im::ReminderActionStatusReport& report);

}  // namespace voicelife::im
