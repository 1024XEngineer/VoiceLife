#include <string>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/linx/linx_speech_provider.h"

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
        if (connect_result.ok() && sink_.on_connected) {
            sink_.on_connected();
        }
        return connect_result;
    }
    Status SendText(std::string_view message) override {
        texts.emplace_back(message);
        if (emit_hello && message.find("\"type\":\"hello\"") != std::string_view::npos && sink_.on_text) {
            sink_.on_text(hello_message);
        }
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
    void EmitConnected() {
        if (sink_.on_connected) {
            sink_.on_connected();
        }
    }
    void EmitDisconnected() {
        if (sink_.on_disconnected) {
            sink_.on_disconnected();
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
    std::string hello_message =
        R"({"type":"hello","transport":"websocket","session_id":"remote-linx-session","audio_params":{"format":"pcm","sample_rate":24000,"channels":1,"bit_depth":16,"frame_duration":60}})";
    bool emit_hello = true;
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
    Check(hello.value->find("\"transport\":\"websocket\"") != std::string::npos, "hello 必须声明 websocket transport");
    Check(hello.value->find("\"mcp\":true") != std::string::npos, "hello 必须声明 MCP 能力");
    Check(hello.value->find("\"sample_rate\":16000") != std::string::npos, "hello 必须声明采样率");
    auto detect = codec.EncodeListenDetect(config, "请播报\\测试", "收到！");
    Check(detect.ok() && detect.value->find("\\\\测试") != std::string::npos &&
              detect.value->find("\"text_response\":\"收到！\"") != std::string::npos,
          "detect 必须正确转义文本并携带受控 TTS 请求");
    Check(codec.EncodeListenDetect(config, "").status.code == ErrorCode::kInvalidArgument, "空 detect 文本必须拒绝");

    auto parsed_hello = codec.DecodeText(
        R"({"type":"hello","transport":"websocket","session_id":"remote",
           "audio_params":{"format":"pcm","sample_rate":16000,"channels":1,"bit_depth":16}})");
    Check(parsed_hello.ok() && parsed_hello.value->audio_params.has_value(), "hello 响应应解析音频参数");
    auto parsed_sentence = codec.DecodeText(R"({"type":"tts","state":"sentence_start","text":"好的，已创建。"})");
    Check(parsed_sentence.ok() && parsed_sentence.value->tts_state == voicelife::linx::LinxTtsState::kSentenceStart,
          "tts sentence_start 应映射");
    auto parsed_stop = codec.DecodeText(R"({"type":"tts","state":"stop","is_aborted":true})");
    Check(parsed_stop.ok() && parsed_stop.value->aborted, "tts stop 应保留 is_aborted");
    auto parsed_mcp = codec.DecodeText(R"({"type":"mcp","payload":{"jsonrpc":"2.0","method":"tools/list","id":1}})");
    Check(parsed_mcp.ok() && parsed_mcp.value->kind == voicelife::linx::LinxMessageKind::kMcp &&
              parsed_mcp.value->text.find("tools/list") != std::string::npos,
          "MCP 消息应保留 JSON-RPC payload");
    auto parsed_goodbye = codec.DecodeText(R"({"type":"goodbye","message":"session closed"})");
    Check(parsed_goodbye.ok() && parsed_goodbye.value->kind == voicelife::linx::LinxMessageKind::kGoodbye &&
              parsed_goodbye.value->text == "session closed",
          "goodbye 消息应解析为会话告别类型");
    auto parsed_llm = codec.DecodeText(R"({"type":"llm","text":"ok","emotion":"happy","action":"thinking"})");
    Check(parsed_llm.ok() && parsed_llm.value->kind == voicelife::linx::LinxMessageKind::kLlm &&
              parsed_llm.value->emotion.has_value() && *parsed_llm.value->emotion == "happy" &&
              parsed_llm.value->action.has_value() && *parsed_llm.value->action == "thinking",
          "llm 表情消息应解析 emotion/action");
    Check(codec.DecodeText(R"({"type":"mystery"})").status.code == ErrorCode::kInvalidArgument, "未知消息类型必须拒绝");

    FakeTransport transport;
    voicelife::linx::LinxSpeechProviderAdapter provider(transport, codec, connection);
    std::vector<voicelife::voice::VoiceEvent> events;
    std::vector<voicelife::voice::AudioFrame> received_audio;
    bool reject_output = false;
    provider.SetAudioSink([&received_audio, &reject_output](voicelife::voice::AudioFrame frame) {
        if (reject_output) return Status::Error(ErrorCode::kConflict, "测试播放队列已满");
        received_audio.push_back(std::move(frame));
        return Status::Ok();
    });
    voicelife::voice::VoiceSessionConfig session_config = config;
    session_config.generation = 7;
    Check(
        provider
            .Connect(session_config, [&events](const voicelife::voice::VoiceEvent& event) { events.push_back(event); })
            .ok(),
        "Provider 应先连接传输并发送 hello");
    Check(transport.connects == 1 && transport.texts.size() == 1 &&
              transport.texts.front().find("\"type\":\"hello\"") != std::string::npos,
          "连接必须只发送一次 hello");
    Check(!events.empty() && events.back().kind == voicelife::voice::VoiceEventKind::kConnected &&
              events.back().generation == 7,
          "hello 事件必须携带当前 generation");
    auto formats = provider.audio_formats();
    Check(formats.ok() && formats.value->capture.sample_rate_hz == 16000 &&
              formats.value->playback.sample_rate_hz == 24000 && formats.value->playback.frame_duration_ms == 60,
          "Provider 应分别暴露请求的上行格式和 hello 协商的下行格式");
    transport.EmitConnected();
    Check(transport.texts.size() == 1, "重复 connected 事件不得重复发送 hello");
    Check(provider.NotifyLocalWakeWord("你好牛牛", "收到！").ok() && provider.StartCapture(config.mode).ok() &&
              provider.StopCapture().ok(),
          "本地唤醒确认必须先 detect，再发送 listen start/stop");
    Check(provider.Speak("测试播报").ok() && provider.Abort("user_interrupt").ok(), "detect/abort 应通过传输发送");
    Check(transport.texts.size() == 6, "hello、本地 detect、listen、listen、detect、abort 应各发送一帧");
    Check(transport.texts[1].find("\"type\":\"listen\"") != std::string::npos &&
              transport.texts[1].find("\"state\":\"detect\"") != std::string::npos &&
              transport.texts[1].find("\"text\":\"你好牛牛\"") != std::string::npos &&
              transport.texts[1].find("\"text_response\":\"收到！\"") != std::string::npos &&
              transport.texts[2].find("\"state\":\"start\"") != std::string::npos,
          "本地唤醒链路必须保持带收到播报的 listen.detect 在 listen.start 之前");
    Check(transport.texts[4].find("\"text\":\"system_prompt\"") != std::string::npos &&
              transport.texts[4].find("\"text_response\":\"测试播报\"") != std::string::npos,
          "系统播报必须使用 Linx 定义的 text_response，不能伪装为用户 STT");
    Check(transport.texts[1].find("\"session_id\":\"remote-linx-session\"") != std::string::npos &&
              transport.texts[5].find("\"session_id\":\"remote-linx-session\"") != std::string::npos,
          "服务端 hello 分配的 session_id 必须用于后续控制消息");
    const auto events_before_mismatched_session = events.size();
    transport.EmitText(R"({"type":"stt","session_id":"wrong-session","text":"不应接受"})");
    Check(events.size() == events_before_mismatched_session + 1 &&
              events.back().kind == voicelife::voice::VoiceEventKind::kError,
          "后续来自其他 session 的消息必须拒绝");

    const auto events_before_goodbye = events.size();
    transport.EmitText(R"({"type":"goodbye","session_id":"remote-linx-session","message":"bye"})");
    const auto events_after_goodbye = events.size();
    transport.EmitText(
        R"({"type":"llm","session_id":"remote-linx-session","text":"ok","emotion":"happy","action":"thinking"})");
    Check(events_after_goodbye == events_before_goodbye && events.size() == events_after_goodbye,
          "goodbye 与 llm 表情消息不得触发 provider 错误事件");
    Check(provider.audio_formats().ok(), "goodbye/llm 消息不得破坏已协商的音频格式");

    FakeTransport mcp_transport;
    int mcp_calls = 0;
    voicelife::linx::LinxSpeechProviderAdapter mcp_provider(
        mcp_transport, codec, connection, voicelife::linx::LinxSpeechProviderAdapter::DefaultCapabilities(),
        [&mcp_calls](std::string_view, std::string_view session_id) {
            ++mcp_calls;
            return voicelife::Result<std::string>::Success(
                "{\"type\":\"mcp\",\"session_id\":\"" + std::string(session_id) +
                "\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}}");
        });
    Check(mcp_provider.Connect(session_config, {}).ok(), "配置 MCP handler 的 Provider 应连接成功");
    std::vector<voicelife::voice::VoiceEvent> mcp_events;
    mcp_provider.Disconnect();
    Check(mcp_provider
              .Connect(session_config,
                       [&mcp_events](const voicelife::voice::VoiceEvent& event) { mcp_events.push_back(event); })
              .ok(),
          "配置 MCP handler 的 Provider 应能重新绑定事件接收器");
    const auto mcp_events_before_request = mcp_events.size();
    mcp_transport.EmitText(R"({"type":"mcp","payload":{"jsonrpc":"2.0","method":"tools/list","id":1}})");
    Check(mcp_calls == 1 && mcp_transport.texts.back().find("\"type\":\"mcp\"") != std::string::npos &&
              mcp_transport.texts.back().find("\"session_id\":\"remote-linx-session\"") != std::string::npos,
          "MCP payload 应调用 handler 并回发响应");
    Check(mcp_events.size() == mcp_events_before_request,
          "MCP 网络回调只能交给受控 handler，不能直接投递可绕过 Runtime 的工具事件");

    voicelife::voice::AudioFrame uplink;
    uplink.generation = 7;
    uplink.sequence = 0;
    uplink.format = config.audio;
    uplink.payload = {1, 2, 3};
    Check(provider.SendAudio(uplink).ok() && transport.audio_frames.size() == 1, "当前 generation 音频应上行");
    transport.EmitBinary({4, 5, 6});
    Check(received_audio.size() == 1 && received_audio.front().generation == 7 &&
              received_audio.front().sequence == 0 && received_audio.front().payload.size() == 3 &&
              received_audio.front().format.sample_rate_hz == 24000,
          "二进制下行音频应使用协商格式并携带 generation");
    const auto events_before_output_backpressure = events.size();
    reject_output = true;
    transport.EmitBinary({5, 6, 7});
    reject_output = false;
    Check(events.size() == events_before_output_backpressure,
          "有界播放队列拒绝单帧应只计入端口指标，不能伪装成 Provider 失败");
    transport.EmitDisconnected();
    Check(events.back().kind == voicelife::voice::VoiceEventKind::kDisconnected, "物理断线必须向会话上报生命周期事件");
    Check(provider.SendAudio(uplink).code == ErrorCode::kUnavailable, "断线后必须立即阻断音频上行");
    provider.SetGeneration(8);
    transport.EmitConnected();
    Check(transport.texts.size() == 7 && transport.texts.back().find("\"type\":\"hello\"") != std::string::npos,
          "自动重连后必须只补发一次 hello");
    Check(events.back().kind == voicelife::voice::VoiceEventKind::kConnected && events.back().generation == 8,
          "重连 hello 必须使用新的 generation");
    transport.EmitBinary({7, 8, 9});
    Check(received_audio.size() == 2 && received_audio.back().generation == 8 && received_audio.back().sequence == 0,
          "同一连接打断后 Provider 应切换到新的 generation");
    uplink.generation = 6;
    Check(provider.SendAudio(uplink).code == ErrorCode::kConflict, "旧 generation 上行必须拒绝");

    transport.EmitDisconnected();
    provider.SetGeneration(9);
    transport.hello_message =
        R"({"type":"hello","transport":"websocket","audio_params":{"format":"pcm","sample_rate":16000,"channels":1,"bit_depth":16,"frame_duration":20}})";
    transport.EmitConnected();
    Check(events.back().kind == voicelife::voice::VoiceEventKind::kError && events.back().generation == 9,
          "重连改变下行格式时必须上报错误，不得假装 ready");
    Check(provider.audio_formats().status.code == ErrorCode::kUnavailable,
          "重连改变下行格式后不得继续暴露旧的协商格式");
    Check(provider.StartCapture(config.mode).code == ErrorCode::kUnavailable, "重连改变下行格式后必须阻断上行");
    Check(provider.Disconnect().ok() && transport.closes == 1, "断开应关闭传输并清理回调");

    FakeTransport failed_transport;
    failed_transport.connect_result = Status::Error(ErrorCode::kUnavailable, "网络不可用");
    voicelife::linx::LinxSpeechProviderAdapter failed_provider(failed_transport, codec, connection);
    Check(failed_provider.Connect(session_config, {}).code == ErrorCode::kUnavailable, "传输连接失败应向上传播");
    Check(failed_provider.StartCapture(config.mode).code == ErrorCode::kUnavailable, "连接失败后不能发送 listen");

    FakeTransport timeout_transport;
    timeout_transport.emit_hello = false;
    voicelife::linx::LinxSpeechProviderAdapter timeout_provider(timeout_transport, codec, connection);
    auto timeout_config = session_config;
    timeout_config.hello_timeout_ms = 5;
    Check(timeout_provider.Connect(timeout_config, {}).code == ErrorCode::kUnavailable,
          "未收到 Linx hello 必须在超时后失败");
    Check(timeout_provider.Connect(timeout_config, {}).code == ErrorCode::kConflict,
          "hello 失败但物理连接尚未完成清理时不得重复 Connect");

    // 补充错误路径与边界覆盖,提升 patch 覆盖率。
    auto invalid_audio = config;
    invalid_audio.audio.sample_rate_hz = 0;
    Check(codec.EncodeHello(invalid_audio, connection).status.code == ErrorCode::kInvalidArgument,
          "hello 必须拒绝无效音频参数");
    Check(codec.EncodeAbort(config, "").status.code == ErrorCode::kInvalidArgument, "空 abort 原因必须拒绝");
    Check(codec.DecodeText("not-json").status.code == ErrorCode::kInvalidArgument, "非 JSON 输入必须拒绝");
    Check(codec.DecodeText(R"({"type":123})").status.code == ErrorCode::kInvalidArgument, "type 非字符串必须拒绝");
    Check(codec.DecodeText(R"({"type":"stt"})").status.code == ErrorCode::kInvalidArgument, "stt 缺少 text 必须拒绝");
    Check(codec.DecodeText(R"({"type":"tts","state":"unknown"})").status.code == ErrorCode::kInvalidArgument,
          "未知 tts 状态必须拒绝");
    Check(codec.DecodeText(R"({"type":"hello","transport":"tcp"})").status.code == ErrorCode::kInvalidArgument,
          "非 websocket transport 必须拒绝");
    Check(codec.DecodeText(R"({"type":"hello","audio_params":{"format":"pcm","sample_rate":16000,"channels":0}})")
                  .status.code == ErrorCode::kInvalidArgument,
          "超出范围的音频参数必须拒绝");
    Check(codec.DecodeText(R"({"type":"error","message":"boom"})").value->kind ==
              voicelife::linx::LinxMessageKind::kError,
          "error 消息应解析 message");
    Check(codec.DecodeText(R"({"type":"stt","text":"听写"})").value->kind == voicelife::linx::LinxMessageKind::kStt,
          "stt 消息应解析 text");
    Check(codec.DecodeText("{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"x\\uy\"}").status.code ==
              ErrorCode::kInvalidArgument,
          "\\u 转义在便携 codec 中必须拒绝");
    Check(codec.DecodeText(R"({"type":"hello","audio_params":{"format":"wav","sample_rate":16000,"channels":1}})")
                  .status.code == ErrorCode::kInvalidArgument,
          "不支持的音频格式必须拒绝");
    Check(codec.DecodeText(R"({"type":"hello","audio_params":{"format":"pcm","sample_rate":16000.5,"channels":1}})")
                  .status.code == ErrorCode::kInvalidArgument,
          "非整数采样率必须拒绝");
    Check(
        codec.DecodeText(
                 R"({"type":"hello","audio_params":{"format":"pcm","sample_rate":16000,"channels":1,"bit_depth":300}})")
                .status.code == ErrorCode::kInvalidArgument,
        "超范围位深必须拒绝");
    return 0;
}
