#pragma once

#include <string>

#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/contracts/im/reminder_action_result.h"
#include "voicelife/contracts/im/schedule_receipt.h"

namespace voicelife::im {

/// 把日程操作回执序列化为网关契约 JSON 文本。
std::string SerializeScheduleReceiptIntent(const contracts::im::ScheduleReceiptIntent& intent);
/// 把提醒通知意图序列化为网关契约 JSON 文本。
std::string SerializeNotificationIntent(const contracts::im::NotificationIntent& intent);
/// 把提醒动作执行结果序列化为网关契约 JSON 文本。
std::string SerializeReminderActionResult(const contracts::im::ReminderActionResult& result);

}  // namespace voicelife::im
