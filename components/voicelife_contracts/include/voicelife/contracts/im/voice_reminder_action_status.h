#pragma once

#include <optional>
#include <string>

#include "im_contracts.h"
#include "voicelife/contracts/json.h"

namespace voicelife::contracts::im {

/// 设备本地语音动作已提交后的跨入口状态事实。
struct VoiceReminderActionStatus {
    std::string schemaVersion;
    std::string eventId;
    std::string correlationId;
    std::string deviceId;
    std::string reminderTriggerId;
    std::string operationId;
    std::string action;
    std::string status;
    std::string occurredAt;
    std::optional<std::string> nextTriggerAt;
    std::string source;
};

Status ParseVoiceReminderActionStatus(const JsonValue& root, VoiceReminderActionStatus& out);
}  // namespace voicelife::contracts::im
