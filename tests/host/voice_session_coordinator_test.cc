#include "voicelife/voice/voice_session_coordinator.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolCall;
using voicelife::ToolResult;
using voicelife::test::Check;

namespace {

class FakeAudio final : public voicelife::voice::AudioDevicePort {
   public:
    Status Open() override {
        ++opens;
        return result;
    }
    void Close() override { ++closes; }

    Status result = Status::Ok();
    int opens = 0;
    int closes = 0;
};

class FakeSpeech final : public voicelife::voice::SpeechProviderPort {
   public:
    Status Connect() override {
        ++connects;
        return result;
    }
    void Disconnect() override { ++disconnects; }

    Status result = Status::Ok();
    int connects = 0;
    int disconnects = 0;
};

class RecordingTools final : public voicelife::voice::ToolGatewayPort {
   public:
    ToolResult Call(const ToolCall&) override {
        ++calls;
        return {.status = Status::Ok(), .output = {}};
    }
    int calls = 0;
};

}  // namespace

int main() {
    FakeAudio audio;
    FakeSpeech speech;
    RecordingTools tools;
    voicelife::voice::VoiceSessionCoordinator voice(audio, speech, tools);

    Check(voice.DispatchToolCall({}).status.code == ErrorCode::kUnavailable, "会话就绪前不能分发工具调用");
    Check(voice.Start().ok(), "音频和语音服务就绪时会话应启动");
    Check(voice.Start().ok() && audio.opens == 1 && speech.connects == 1, "重复启动应保持幂等");
    Check(voice.DispatchToolCall({}).status.ok() && tools.calls == 1, "就绪会话应转发工具调用");
    voice.Stop();
    Check(audio.closes == 1 && speech.disconnects == 1, "停止会话应释放已打开的资源");

    FakeAudio unavailable_audio;
    unavailable_audio.result = Status::Error(ErrorCode::kUnavailable, "音频不可用");
    FakeSpeech unused_speech;
    RecordingTools unused_tools;
    voicelife::voice::VoiceSessionCoordinator audio_failure(unavailable_audio, unused_speech, unused_tools);
    Check(audio_failure.Start().code == ErrorCode::kUnavailable, "音频启动失败应向上传播");
    Check(unused_speech.connects == 0, "音频失败后不能继续连接语音服务");

    FakeAudio opened_audio;
    FakeSpeech unavailable_speech;
    unavailable_speech.result = Status::Error(ErrorCode::kUnavailable, "语音服务不可用");
    voicelife::voice::VoiceSessionCoordinator speech_failure(opened_audio, unavailable_speech, unused_tools);
    Check(speech_failure.Start().code == ErrorCode::kUnavailable, "语音服务失败应向上传播");
    Check(opened_audio.closes == 1, "语音服务失败时应回滚已打开的音频资源");
    return 0;
}
