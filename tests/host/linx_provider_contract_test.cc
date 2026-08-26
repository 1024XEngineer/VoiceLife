#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
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
        if (text_sent_callback) text_sent_callback();
        return send_text_result;
    }
    Status SendAudio(voicelife::voice::AudioFrame frame) override {
        if (frame.generation != tx_generation) {
            return Status::Error(ErrorCode::kConflict, "测试 TX gate 拒绝非当前 generation 音频");
        }
        audio_frames.push_back(std::move(frame));
        return send_audio_result;
    }
    Status Close() override {
        ++closes;
        return close_result;
    }
    void SetGeneration(uint64_t generation) override { tx_generation = generation; }

    void EmitText(std::string message) {
        if (sink_.on_text) {
            sink_.on_text(message);
        }
    }
    void EmitBinary(std::vector<uint8_t> payload) {
        if (sink_.on_binary) {
            emitted_binary_data = payload.data();
            sink_.on_binary(std::move(payload));
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
    uint64_t tx_generation = 0;
    const uint8_t* emitted_binary_data = nullptr;
    std::function<void()> text_sent_callback;
};

class FakeOpusCodecStrategy final : public voicelife::voice::CodecStrategy {
   public:
    [[nodiscard]] voicelife::voice::AudioCodec codec() const override { return voicelife::voice::AudioCodec::kOpus; }

    Status Configure(const voicelife::voice::AudioFormat& local_pcm,
                     const voicelife::voice::AudioFormat& wire) override {
        ++configure_calls;
        local_ = local_pcm;
        wire_ = wire;
        return configure_status;
    }

    voicelife::Result<voicelife::voice::AudioFrame> Encode(const voicelife::voice::AudioFrame& pcm) override {
        ++encode_calls;
        if (pcm.format.codec != voicelife::voice::AudioCodec::kPcmS16Le) {
            return voicelife::Result<voicelife::voice::AudioFrame>::Failure(ErrorCode::kInvalidArgument,
                                                                            "测试 PCM 格式错误");
        }
        voicelife::voice::AudioFrame encoded;
        encoded.generation = pcm.generation;
        encoded.sequence = pcm.sequence;
        encoded.format = wire_;
        encoded.payload = {0xF8, 0x01};
        return voicelife::Result<voicelife::voice::AudioFrame>::Success(std::move(encoded));
    }

    voicelife::Result<voicelife::voice::AudioFrame> Decode(const voicelife::voice::AudioFrame& encoded) override {
        ++decode_calls;
        if (!decode_status.ok()) {
            return voicelife::Result<voicelife::voice::AudioFrame>::Failure(decode_status.code, decode_status.message);
        }
        if (encoded.format.codec != voicelife::voice::AudioCodec::kOpus) {
            return voicelife::Result<voicelife::voice::AudioFrame>::Failure(ErrorCode::kInvalidArgument,
                                                                            "测试 Opus 格式错误");
        }
        voicelife::voice::AudioFrame pcm;
        pcm.generation = encoded.generation;
        pcm.sequence = encoded.sequence;
        pcm.format = local_;
        pcm.payload = {1, 2, 3};
        return voicelife::Result<voicelife::voice::AudioFrame>::Success(std::move(pcm));
    }

    Status configure_status = Status::Ok();
    Status decode_status = Status::Ok();
    int configure_calls = 0;
    int encode_calls = 0;
    int decode_calls = 0;

   private:
    voicelife::voice::AudioFormat local_;
    voicelife::voice::AudioFormat wire_;
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
            .agent_id = std::string("agent-test"),
            .preferred_audio = std::nullopt};
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
    Check(hello.value->find("\"play_buffer_duration\":1000") != std::string::npos,
          "默认播放缓冲必须符合 Linx 文档的 1000ms 协议值");
    auto larger_buffer_connection = connection;
    larger_buffer_connection.playback_buffer_duration_ms = 320;
    auto larger_buffer_hello = codec.EncodeHello(config, larger_buffer_connection);
    Check(larger_buffer_hello.ok() &&
              larger_buffer_hello.value->find("\"play_buffer_duration\":320") != std::string::npos,
          "连接配置必须能显式控制 Linx 下行缓冲预算");
    auto opus_connection = connection;
    opus_connection.preferred_audio = {.codec = voicelife::voice::AudioCodec::kOpus,
                                       .sample_rate_hz = 16000,
                                       .channels = 1,
                                       .bits_per_sample = 16,
                                       .frame_duration_ms = 20};
    auto opus_hello = codec.EncodeHello(config, opus_connection);
    Check(opus_hello.ok() && opus_hello.value->find("\"format\":\"opus\"") != std::string::npos &&
              opus_hello.value->find("\"play_buffer_duration\":1000") != std::string::npos &&
              opus_hello.value->find("\"bit_depth\"") == std::string::npos,
          "Opus hello 必须保留播放缓冲预算且不得伪造 PCM 字段");
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
    Check(transport.tx_generation == session_config.generation,
          "首次连接必须在首帧 PCM 进入 Transport 前初始化 TX generation");
    Check(!events.empty() && events.back().kind == voicelife::voice::VoiceEventKind::kConnected &&
              events.back().generation == 7,
          "hello 事件必须携带当前 generation");
    auto formats = provider.audio_formats();
    Check(formats.ok() && formats.value->capture.sample_rate_hz == 16000 &&
              formats.value->playback.sample_rate_hz == 24000 && formats.value->playback.frame_duration_ms == 60,
          "Provider 应分别暴露请求的上行格式和 hello 协商的下行格式");
    transport.EmitConnected();
    Check(transport.texts.size() == 1, "重复 connected 事件不得重复发送 hello");
    Check(provider.NotifyLocalWakeWord("你好牛牛").ok() && provider.StartCapture(config.mode).ok() &&
              provider.StopCapture().ok(),
          "普通本地唤醒必须先发送无确认音 detect，再由状态机开始采集");
    Check(provider.Speak("测试播报").ok() && provider.Abort("user_interrupt").ok(), "detect/abort 应通过传输发送");
    Check(transport.texts.size() == 6, "hello、本地 detect、listen、listen、detect、abort 应各发送一帧");
    Check(transport.texts[1].find("\"type\":\"listen\"") != std::string::npos &&
              transport.texts[1].find("\"state\":\"detect\"") != std::string::npos &&
              transport.texts[1].find("\"text\":\"你好牛牛\"") != std::string::npos &&
              transport.texts[1].find("\"text_response\"") == std::string::npos &&
              transport.texts[2].find("\"state\":\"start\"") != std::string::npos,
          "普通本地唤醒 detect 不得请求确认播报；listen.start 由后续状态机控制");
    Check(transport.texts[4].find("\"text\":\"system_prompt\"") != std::string::npos &&
              transport.texts[4].find("\"text_response\":\"测试播报\"") != std::string::npos,
          "系统播报必须使用 Linx 定义的 text_response，不能伪装为用户 STT");
    Check(transport.texts[1].find("\"session_id\":\"remote-linx-session\"") != std::string::npos &&
              transport.texts[5].find("\"session_id\":\"remote-linx-session\"") != std::string::npos,
          "服务端 hello 分配的 session_id 必须用于后续控制消息");
    Check(transport.texts[3].find("\"state\":\"stop\"") != std::string::npos &&
              transport.texts[3].find("\"mode\"") == std::string::npos,
          "listen.stop 必须遵循 Linx 文档，不携带 listen.start 的 mode");
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
    std::mutex mcp_test_mutex;
    std::condition_variable mcp_test_cv;
    bool mcp_started = false;
    bool mcp_release = false;
    bool mcp_response_sent = false;
    std::vector<voicelife::voice::AudioFrame> mcp_audio;
    voicelife::linx::LinxSpeechProviderAdapter mcp_provider(
        mcp_transport, codec, connection, voicelife::linx::LinxSpeechProviderAdapter::DefaultCapabilities(),
        [&mcp_calls, &mcp_test_mutex, &mcp_test_cv, &mcp_started, &mcp_release](std::string_view,
                                                                                std::string_view session_id) {
            {
                std::unique_lock<std::mutex> lock(mcp_test_mutex);
                ++mcp_calls;
                mcp_started = true;
                mcp_test_cv.notify_all();
                mcp_test_cv.wait(lock, [&mcp_release]() { return mcp_release; });
            }
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
    mcp_provider.SetAudioSink([&mcp_audio](voicelife::voice::AudioFrame frame) {
        mcp_audio.push_back(std::move(frame));
        return Status::Ok();
    });
    mcp_transport.text_sent_callback = [&mcp_test_mutex, &mcp_test_cv, &mcp_response_sent]() {
        {
            std::lock_guard<std::mutex> lock(mcp_test_mutex);
            mcp_response_sent = true;
        }
        mcp_test_cv.notify_all();
    };
    const auto mcp_events_before_request = mcp_events.size();
    mcp_transport.EmitText(R"({"type":"mcp","payload":{"jsonrpc":"2.0","method":"tools/list","id":1}})");
    {
        std::unique_lock<std::mutex> lock(mcp_test_mutex);
        Check(mcp_test_cv.wait_for(lock, std::chrono::seconds(1), [&mcp_started]() { return mcp_started; }),
              "MCP 请求应在专用 worker 中完成，不能阻塞 Linx RX 回调");
    }
    mcp_transport.EmitBinary({9, 8, 7});
    Check(mcp_audio.size() == 1, "MCP handler 等待期间 Linx RX 仍必须继续投递下行音频");
    {
        std::lock_guard<std::mutex> lock(mcp_test_mutex);
        mcp_release = true;
    }
    mcp_test_cv.notify_all();
    {
        std::unique_lock<std::mutex> lock(mcp_test_mutex);
        Check(mcp_test_cv.wait_for(lock, std::chrono::seconds(1), [&mcp_response_sent]() { return mcp_response_sent; }),
              "MCP worker 完成后应回发响应");
    }
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
    const auto* uplink_data = uplink.payload.data();
    Check(provider.SendAudio(std::move(uplink)).ok() && transport.audio_frames.size() == 1 &&
              transport.audio_frames.back().payload.data() == uplink_data,
          "当前 generation 音频应上行");
    Check(transport.audio_frames.back().payload.data() == uplink_data,
          "Provider 到 WebSocket Transport 的上行 PCM 负载必须移动，不能复制每个音频帧");
    transport.EmitBinary({4, 5, 6});
    Check(received_audio.size() == 1 && received_audio.front().generation == 7 &&
              received_audio.front().sequence == 0 && received_audio.front().payload.size() == 3 &&
              received_audio.front().format.sample_rate_hz == 24000 &&
              received_audio.front().payload.data() == transport.emitted_binary_data,
          "二进制下行音频应使用协商格式并携带 generation");
    const auto events_before_output_backpressure = events.size();
    reject_output = true;
    for (int frame = 0; frame < 120; ++frame) {
        transport.EmitBinary({5, 6, 7});
    }
    reject_output = false;
    Check(events.size() == events_before_output_backpressure,
          "长 TTS 的连续播放队列拒绝只能计入端口指标，不能伪装成 Provider 失败或触发重连");
    transport.EmitDisconnected();
    Check(events.back().kind == voicelife::voice::VoiceEventKind::kDisconnected, "物理断线必须向会话上报生命周期事件");
    Check(provider.SendAudio({}).code == ErrorCode::kUnavailable, "断线后必须立即阻断音频上行");
    provider.SetGeneration(8);
    transport.EmitConnected();
    Check(transport.texts.size() == 7 && transport.texts.back().find("\"type\":\"hello\"") != std::string::npos,
          "自动重连后必须只补发一次 hello");
    Check(events.back().kind == voicelife::voice::VoiceEventKind::kConnected && events.back().generation == 8,
          "重连 hello 必须使用新的 generation");
    transport.EmitBinary({7, 8, 9});
    Check(received_audio.size() == 2 && received_audio.back().generation == 8 && received_audio.back().sequence == 0,
          "同一连接打断后 Provider 应切换到新的 generation");
    voicelife::voice::AudioFrame stale_uplink;
    stale_uplink.generation = 6;
    Check(provider.SendAudio(std::move(stale_uplink)).code == ErrorCode::kConflict, "旧 generation 上行必须拒绝");

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

    // Wire Opus must remain entirely inside the provider. VoiceSession and
    // board ports continue to see their original PCM format.
    FakeTransport opus_transport;
    opus_transport.hello_message =
        R"({"type":"hello","transport":"websocket","session_id":"opus-session","audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":20}})";
    auto opus_strategy = std::make_unique<FakeOpusCodecStrategy>();
    auto* opus_strategy_raw = opus_strategy.get();
    voicelife::linx::LinxSpeechProviderAdapter opus_provider(
        opus_transport, codec, opus_connection, voicelife::linx::LinxSpeechProviderAdapter::DefaultCapabilities(), {},
        std::move(opus_strategy));
    std::vector<voicelife::voice::VoiceEvent> opus_events;
    std::vector<voicelife::voice::AudioFrame> opus_received;
    opus_provider.SetAudioSink([&opus_received](voicelife::voice::AudioFrame frame) {
        opus_received.push_back(std::move(frame));
        return Status::Ok();
    });
    Check(opus_provider
                  .Connect(session_config,
                           [&opus_events](const voicelife::voice::VoiceEvent& event) { opus_events.push_back(event); })
                  .ok() &&
              opus_strategy_raw->configure_calls == 1,
          "Opus 策略必须在发送 hello 前配置成功");
    Check(opus_transport.texts.front().find("\"format\":\"opus\"") != std::string::npos &&
              opus_transport.texts.front().find("\"play_buffer_duration\":1000") != std::string::npos,
          "Provider 必须以官方 Opus hello 格式连接");
    auto opus_formats = opus_provider.audio_formats();
    Check(opus_formats.ok() && opus_formats.value->capture.codec == voicelife::voice::AudioCodec::kPcmS16Le &&
              opus_formats.value->playback.codec == voicelife::voice::AudioCodec::kPcmS16Le,
          "Provider 对 VoiceSession 暴露的双向格式必须保持本地 PCM");
    voicelife::voice::AudioFrame opus_uplink;
    opus_uplink.generation = session_config.generation;
    opus_uplink.sequence = 0;
    opus_uplink.format = config.audio;
    opus_uplink.payload = {9, 8, 7};
    Check(opus_provider.SendAudio(std::move(opus_uplink)).ok() && opus_strategy_raw->encode_calls == 1 &&
              opus_transport.audio_frames.size() == 1 &&
              opus_transport.audio_frames.front().format.codec == voicelife::voice::AudioCodec::kOpus &&
              opus_transport.audio_frames.front().payload.size() == 2,
          "PCM 上行必须先编码为 Opus，再交给 WebSocket");
    opus_transport.EmitBinary({0xF8, 0x02});
    Check(opus_strategy_raw->decode_calls == 1 && opus_received.size() == 1 &&
              opus_received.front().format.codec == voicelife::voice::AudioCodec::kPcmS16Le &&
              opus_received.front().payload.size() == 3,
          "WebSocket 下行 Opus 必须先解码为本地 PCM");
    const auto opus_events_before_decode_backpressure = opus_events.size();
    opus_strategy_raw->decode_status = Status::Error(ErrorCode::kConflict, "测试 Opus 下行池已满");
    for (int frame = 0; frame < 120; ++frame) {
        opus_transport.EmitBinary({0xF8, 0x02});
    }
    Check(opus_events.size() == opus_events_before_decode_backpressure,
          "长 TTS 耗尽 Opus 下行池时只能丢帧，不能伪装成 Provider 错误或触发重连");
    Check(opus_provider.Disconnect().ok(), "Opus Provider 应正常清理传输");

    FakeTransport missing_strategy_transport;
    voicelife::linx::LinxSpeechProviderAdapter missing_strategy_provider(missing_strategy_transport, codec,
                                                                         opus_connection);
    Check(missing_strategy_provider.Connect(session_config, {}).code == ErrorCode::kUnavailable &&
              missing_strategy_transport.connects == 0,
          "缺少 Opus 策略时必须在 hello 前拒绝连接，不能假装支持 Opus");

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
    auto invalid_connection = connection;
    invalid_connection.playback_buffer_duration_ms = 0;
    Check(!invalid_connection.valid() &&
              codec.EncodeHello(config, invalid_connection).status.code == ErrorCode::kInvalidArgument,
          "零播放缓冲预算不能生成 Linx hello");
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
