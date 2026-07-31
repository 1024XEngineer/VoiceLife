#include "voicelife_turn_policy.h"

#include <array>

namespace voicelife {
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

}  // namespace

bool ResponseNeedsFollowup(std::string_view text) {
    if (text.find('?') != std::string_view::npos || text.find("？") != std::string_view::npos) {
        return true;
    }

    constexpr std::array<std::string_view, 9> kFollowupPhrases = {
        "是否",   "请确认",   "请选择",   "请指定",       "哪一条",
        "哪一个", "几点结束", "持续多久", "今天还是明天",
    };
    for (std::string_view phrase : kFollowupPhrases) {
        if (text.find(phrase) != std::string_view::npos)
            return true;
    }
    return false;
}

bool ShouldOpenFollowup(bool wake_ack_pending, bool response_needs_followup,
                        bool business_response_completed) {
    return wake_ack_pending || response_needs_followup || business_response_completed;
}

bool ShouldSuppressWakeTranscript(bool wake_ack_pending, std::string_view text) {
    if (!wake_ack_pending) return false;

    constexpr std::array<std::string_view, 10> kSyntheticWakeTranscripts = {
        "嘿，你好呀",   "嘿，你好呀。", "嘿你好呀",   "嘿你好呀。", "你好牛牛",
        "你好牛牛。",   "你好牛牛！",   "你好，牛牛", "你好，牛牛。", "你好，牛牛！",
    };
    for (std::string_view wake_transcript : kSyntheticWakeTranscripts) {
        if (text == wake_transcript) return true;
    }
    return false;
}

bool IsBusinessToolName(std::string_view name) {
    return StartsWith(name, "calendar_") || StartsWith(name, "reminder_") ||
           StartsWith(name, "note_");
}

}  // namespace voicelife
