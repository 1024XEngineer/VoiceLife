#include "support/test_support.h"
#include "voicelife/linx/linx_speech_provider.h"

#include <string>
#include <utility>
#include <vector>

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::test::Check;

namespace {

class FakeTransport final : public voicelife::linx::LinxTransportPort {
   public:
    Status Connect(const voicelife::linx::LinxConnectionConfig& config,
                   voicelife::linx::LinxTransportSink sink) override {
        last_config = config;
        sink_ = std::move(sink);
        ++connects;
        return connect_result;
    }
    Status SendText(std::string_view message) override {
        texts.emplace_back(message);
        return send_text_result;
    }
    Status SendAudio(const voicelife::voice::AudioFrame& frame) override {
        audio_frames.push_back(frame);
        return send_audio_result;
    }
    Status Close() override {
        ++closes;
        return close_result;
    }

    void EmitText(std::string message) {
        if (sink_.on_text) {
            sink_.on_text(message);
        }
    }
    void EmitBinary(std::vector<uint8_t> payload) {
        if (sink_.on_binary) {
            sink_.on_binary(payload);
        }
    }

    voicelife::linx::LinxConnectionConfig last_config;
    voicelife::linx::LinxTransportSink sink_;
    std::vector<std::string> texts;
    std::vector<voicelife::voice::AudioFrame> audio_frames;
    Status connect_result = Status::Ok();
    Status send_text_result = Status::Ok();
    Status send_audio_result = Status::Ok();
    Status close_result = Status::Ok();
    int connects = 0;
    int closes = 0;
};

voicelife::voice::VoiceSessionConfig Config() {
    voicelife::voice::VoiceSessionConfig config;
    config.session_id = "linx-test-session";
    config.provider_id = "xrobot-websocket";
    config.mode = voicelife::voice::VoiceMode::kRealtime;
    return config;
}

voicelife::linx::LinxConnectionConfig Connection() {
    return {.websocket_url = "wss://xrobo-io.qiniuapi.com/v1/ws/",
            .token_ref = "secret://linx/device-token",
            .device_id = "device-test",
            .client_id = "client-test",
            .agent_id = std::string("agent-test")};
}

}  // namespace

int main() {
    voicelife::linx::LinxJsonCodec codec;
    const auto config = Config();
    const auto connection = Connection();

    auto hello = codec.EncodeHello(config, connection);
    Check(hello.ok(), "Linx hello 应可编码");
    Check(hello.value->find("\"transport\":\"websocket\"") != std::string::npos,
          "hello 必须声明 websocket transport");
    Check(hello.value->find("\"sample_rate\":16000") != std::string::npos,
          "hello 必须声明采样率");
    auto detect = codec.EncodeListenDetect(config, "请播报\\测试");
    Check(detect.ok() && detect.value->find("\\\\测试") != std::string::npos,
          "detect 必须正确转义文本并携带请求");
    Check(codec.EncodeListenDetect(config, "").status.code == ErrorCode::kInvalidArgument,
          "空 detect 文本必须拒绝");

    auto parsed_hello = codec.DecodeText(
        R"({"type":"hello","transport":"websocket","session_id":"remote",
           "audio_params":{"format":"pcm","sample_rate":16000,"channels":1,"bit_depth":16}})");
    Check(parsed_hello.ok() && parsed_hello.value->audio_params.has_value(),
          "hello 响应应解析音频参数");
    auto parsed_sentence = codec.DecodeText(
        R"({"type":"tts","state":"sentence_start","text":"好的，已创建。"})");
    Check(parsed_sentence.ok() && parsed_sentence.value->tts_state == voicelife::linx::LinxTtsState::kSentenceStart,
          "tts sentence_start 应映射");
    auto parsed_stop = codec.DecodeText(R"({"type":"tts","state":"stop","is_aborted":true})");
    Check(parsed_stop.ok() && parsed_stop.value->aborted, "tts stop 应保留 is_aborted");
    Check(codec.DecodeText(R"({"type":"mystery"})").status.code == ErrorCode::kInvalidArgument,
          "未知消息类型必须拒绝");

    FakeTransport transport;
    voicelife::linx::LinxSpeechProviderAdapter provider(transport, codec, connection);
    std::vector<voicelife::voice::VoiceEvent> events;
    std::vector<voicelife::voice::AudioFrame> received_audio;
    provider.SetAudioSink([&received_audio](voicelife::voice::AudioFrame frame) {
        received_audio.push_back(std::move(frame));
        return Status::Ok();
    });
    voicelife::voice::VoiceSessionConfig session_config = config;
    session_config.generation = 7;
    Check(provider.Connect(session_config, [&events](const voicelife::voice::VoiceEvent& event) {
              events.push_back(event);
          })
              .ok(),
          "Provider 应先连接传输并发送 hello");
    Check(transport.connects == 1 && transport.texts.size() == 1 &&
              transport.texts.front().find("\"type\":\"hello\"") != std::string::npos,
          "连接必须只发送一次 hello");
    transport.EmitText(R"({"type":"hello","transport":"websocket"})");
    Check(!events.empty() && events.back().kind == voicelife::voice::VoiceEventKind::kConnected &&
              events.back().generation == 7,
          "hello 事件必须携带当前 generation");
    transport.EmitText(
        R"({"type":"hello","transport":"websocket","audio_params":{"format":"pcm","sample_rate":8000,"channels":1}})");
    Check(events.back().kind == voicelife::voice::VoiceEventKind::kError,
          "不支持的音频协商结果必须转为错误事件");
    Check(provider.StartCapture(config.mode).ok() && provider.StopCapture().ok(),
          "listen start/stop 应通过传输发送");
    Check(provider.Speak("测试播报").ok() && provider.Abort("user_interrupt").ok(),
          "detect/abort 应通过传输发送");
    Check(transport.texts.size() == 5, "hello、listen、listen、detect、abort 应各发送一帧");

    voicelife::voice::AudioFrame uplink;
    uplink.generation = 7;
    uplink.sequence = 0;
    uplink.format = config.audio;
    uplink.payload = {1, 2, 3};
    Check(provider.SendAudio(uplink).ok() && transport.audio_frames.size() == 1,
          "当前 generation 音频应上行");
    transport.EmitBinary({4, 5, 6});
    Check(received_audio.size() == 1 && received_audio.front().generation == 7 &&
              received_audio.front().sequence == 0 && received_audio.front().payload.size() == 3,
          "二进制下行音频应映射为带 generation 的 AudioFrame");
    provider.SetGeneration(8);
    transport.EmitBinary({7, 8, 9});
    Check(received_audio.size() == 2 && received_audio.back().generation == 8 &&
              received_audio.back().sequence == 0,
          "同一连接打断后 Provider 应切换到新的 generation");
    uplink.generation = 6;
    Check(provider.SendAudio(uplink).code == ErrorCode::kConflict, "旧 generation 上行必须拒绝");
    Check(provider.Disconnect().ok() && transport.closes == 1, "断开应关闭传输并清理回调");

    FakeTransport failed_transport;
    failed_transport.connect_result = Status::Error(ErrorCode::kUnavailable, "网络不可用");
    voicelife::linx::LinxSpeechProviderAdapter failed_provider(failed_transport, codec, connection);
    Check(failed_provider.Connect(session_config, {}).code == ErrorCode::kUnavailable,
          "传输连接失败应向上传播");
    Check(failed_provider.StartCapture(config.mode).code == ErrorCode::kUnavailable,
          "连接失败后不能发送 listen");
    return 0;
}
