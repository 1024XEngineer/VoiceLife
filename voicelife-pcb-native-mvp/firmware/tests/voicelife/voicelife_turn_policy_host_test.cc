#include "voicelife_turn_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw TestFailure(std::string("line ") + std::to_string(line) + ": " + expression);
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void TestResponseNeedsFollowup() {
    const std::vector<std::string> questions = {
        "时间与已有日程冲突，是否仍要创建？", "持续多久？", "你指今天还是明天？",
        "找到两条日程，请指定要修改哪一条。", "要继续吗?",
    };
    for (const auto& text : questions)
        CHECK(voicelife::ResponseNeedsFollowup(text));

    const std::vector<std::string> final_responses = {
        "",
        "已创建开会，今晚8点。",
        "共有2条安排：今晚9点，开会；今晚10点，开会。",
        "今晚可以好好放松一下啦！",
        "好的，那今晚开会加油哦！",
    };
    for (const auto& text : final_responses)
        CHECK(!voicelife::ResponseNeedsFollowup(text));

    CHECK(voicelife::ShouldOpenFollowup(true, false, false));
    CHECK(voicelife::ShouldOpenFollowup(false, true, false));
    CHECK(voicelife::ShouldOpenFollowup(false, false, true));
    CHECK(!voicelife::ShouldOpenFollowup(false, false, false));
}

void TestBusinessToolClassification() {
    CHECK(voicelife::IsBusinessToolName("calendar_find"));
    CHECK(voicelife::IsBusinessToolName("calendar_modify"));
    CHECK(voicelife::IsBusinessToolName("reminder_snooze"));
    CHECK(voicelife::IsBusinessToolName("note_record"));
    CHECK(!voicelife::IsBusinessToolName("self.get_device_status"));
    CHECK(!voicelife::IsBusinessToolName("self.audio_speaker.set_volume"));
    CHECK(!voicelife::IsBusinessToolName(""));
}

void TestSyntheticWakeTranscriptSuppression() {
    const std::vector<std::string> synthetic_wake_transcripts = {
        "嘿，你好呀", "嘿，你好呀。", "嘿你好呀", "你好牛牛", "你好牛牛！", "你好，牛牛。",
    };
    for (const auto& text : synthetic_wake_transcripts) {
        CHECK(voicelife::ShouldSuppressWakeTranscript(true, text));
        CHECK(!voicelife::ShouldSuppressWakeTranscript(false, text));
    }

    CHECK(!voicelife::ShouldSuppressWakeTranscript(true, "一分钟后提醒我戴耳机。"));
    CHECK(!voicelife::ShouldSuppressWakeTranscript(true, "你好牛牛，今晚七点提醒我开会。"));
}

}  // namespace

int main() {
    try {
        TestResponseNeedsFollowup();
        std::cout << "PASS response follow-up classification\n";
        TestBusinessToolClassification();
        std::cout << "PASS VoiceLife business tool classification\n";
        TestSyntheticWakeTranscriptSuppression();
        std::cout << "PASS synthetic wake transcript suppression\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
