#pragma once

#include <string_view>

namespace voicelife {

bool ResponseNeedsFollowup(std::string_view text);
bool ShouldOpenFollowup(bool wake_ack_pending, bool response_needs_followup,
                        bool business_response_completed);
bool ShouldSuppressWakeTranscript(bool wake_ack_pending, std::string_view text);
bool IsBusinessToolName(std::string_view name);

}  // namespace voicelife
