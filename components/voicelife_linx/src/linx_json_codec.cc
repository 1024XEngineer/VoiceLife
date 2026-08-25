#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "cJSON.h"
#include "voicelife/linx/linx_types.h"

namespace voicelife::linx {
namespace {

// ---- Helpers for building JSON with cJSON ----

// RAII wrapper so every code path deletes the cJSON root.
struct JsonDeleter {
    void operator()(cJSON* ptr) const {
        if (ptr != nullptr) cJSON_Delete(ptr);
    }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

// Caller takes ownership of the returned string (must cJSON_free).
char* PrintUnformatted(cJSON* root) {
    char* printed = cJSON_PrintUnformatted(root);
    return printed != nullptr ? printed : nullptr;
}

Status StringToResult(char* cjson_string, std::string& out) {
    if (cjson_string == nullptr) {
        return Status::Error(ErrorCode::kInternal, "cJSON 序列化失败");
    }
    out.assign(cjson_string);
    cJSON_free(cjson_string);
    return Status::Ok();
}

const char* CodecName(voice::AudioCodec codec) { return codec == voice::AudioCodec::kOpus ? "opus" : "pcm"; }

const char* ModeName(voice::VoiceMode mode) {
    switch (mode) {
        case voice::VoiceMode::kManual:
            return "manual";
        case voice::VoiceMode::kAuto:
            return "auto";
        case voice::VoiceMode::kRealtime:
            return "realtime";
    }
    return "manual";
}

// ---- Helpers for decoding with cJSON ----

const cJSON* GetRequired(const cJSON* root, const char* key, int expected_type, std::string& error) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (item == nullptr || (item->type & expected_type) == 0) {
        error = "缺少或类型错误的字段: ";
        error += key;
        return nullptr;
    }
    return item;
}

const cJSON* GetOptional(const cJSON* root, const char* key, int expected_type) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    return (item != nullptr && (item->type & expected_type) != 0) ? item : nullptr;
}

Result<LinxAudioParams> ParseAudioParams(const cJSON* audio) {
    std::string error;
    const cJSON* fmt = GetRequired(audio, "format", cJSON_String, error);
    const cJSON* sr = GetRequired(audio, "sample_rate", cJSON_Number, error);
    const cJSON* ch = GetRequired(audio, "channels", cJSON_Number, error);
    if (fmt == nullptr || sr == nullptr || ch == nullptr) {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, error);
    }
    LinxAudioParams params;
    const std::string format_str = cJSON_GetStringValue(fmt);
    if (format_str == "pcm") {
        params.codec = voice::AudioCodec::kPcmS16Le;
    } else if (format_str == "opus") {
        params.codec = voice::AudioCodec::kOpus;
    } else {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, "不支持的 Linx 音频格式: " + format_str);
    }
    if (!std::isfinite(sr->valuedouble) || std::floor(sr->valuedouble) != sr->valuedouble || sr->valuedouble < 1 ||
        sr->valuedouble > 0xFFFFFFFFULL || !std::isfinite(ch->valuedouble) ||
        std::floor(ch->valuedouble) != ch->valuedouble || ch->valuedouble < 1 || ch->valuedouble > 255) {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, "Linx 音频参数超出范围");
    }
    const uint32_t sample_rate = static_cast<uint32_t>(sr->valuedouble);
    const uint8_t channels = static_cast<uint8_t>(ch->valuedouble);
    params.sample_rate_hz = sample_rate;
    params.channels = channels;
    const cJSON* bit = GetOptional(audio, "bit_depth", cJSON_Number);
    if (bit != nullptr && (!std::isfinite(bit->valuedouble) || std::floor(bit->valuedouble) != bit->valuedouble ||
                           bit->valuedouble < 1 || bit->valuedouble > 255)) {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, "Linx bit_depth 超出范围");
    }
    if (bit != nullptr) params.bits_per_sample = static_cast<uint8_t>(bit->valuedouble);
    const cJSON* dur = GetOptional(audio, "frame_duration", cJSON_Number);
    if (dur != nullptr && (!std::isfinite(dur->valuedouble) || std::floor(dur->valuedouble) != dur->valuedouble ||
                           dur->valuedouble < 1 || dur->valuedouble > 65535)) {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, "Linx frame_duration 超出范围");
    }
    if (dur != nullptr) params.frame_duration_ms = static_cast<uint16_t>(dur->valuedouble);
    return Result<LinxAudioParams>::Success(params);
}

}  // namespace

// ---- Encode methods: build JSON with cJSON_CreateObject ----

Result<std::string> LinxJsonCodec::EncodeHello(const voice::VoiceSessionConfig& config,
                                               const LinxConnectionConfig& connection) const {
    if (!config.audio.valid() || !connection.valid()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx hello 音频参数无效");
    }
    const voice::AudioFormat wire_audio = connection.preferred_audio.value_or(config.audio);
    if (!wire_audio.valid()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx hello 线上音频参数无效");
    }
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "hello");
    cJSON_AddNumberToObject(root.get(), "version", 1);
    cJSON* features = cJSON_AddObjectToObject(root.get(), "features");
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddStringToObject(root.get(), "transport", "websocket");

    cJSON* audio = cJSON_AddObjectToObject(root.get(), "audio_params");
    cJSON_AddStringToObject(audio, "format", CodecName(wire_audio.codec));
    cJSON_AddNumberToObject(audio, "sample_rate", wire_audio.sample_rate_hz);
    cJSON_AddNumberToObject(audio, "channels", wire_audio.channels);
    cJSON_AddNumberToObject(audio, "frame_duration", wire_audio.frame_duration_ms);
    cJSON_AddNumberToObject(audio, "play_buffer_duration", connection.playback_buffer_duration_ms);

    if (wire_audio.codec == voice::AudioCodec::kPcmS16Le) {
        cJSON_AddNumberToObject(audio, "bit_depth", wire_audio.bits_per_sample);
        cJSON_AddStringToObject(audio, "endianness", "little");
        const uint32_t frame_size = wire_audio.sample_rate_hz * wire_audio.frame_duration_ms / 1000U;
        cJSON_AddNumberToObject(audio, "frame_size", frame_size);
        cJSON_AddStringToObject(audio, "sample_format", "signed_int16");
    }

    std::string result;
    char* printed = PrintUnformatted(root.get());
    Status status = StringToResult(printed, result);
    return status.ok() ? Result<std::string>::Success(std::move(result))
                       : Result<std::string>::Failure(status.code, status.message);
}

Result<std::string> LinxJsonCodec::EncodeListenStart(const voice::VoiceSessionConfig& config) const {
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "listen");
    cJSON_AddStringToObject(root.get(), "state", "start");
    cJSON_AddStringToObject(root.get(), "mode", ModeName(config.mode));
    if (!config.session_id.empty()) {
        cJSON_AddStringToObject(root.get(), "session_id", config.session_id.c_str());
    }
    std::string result;
    char* printed = PrintUnformatted(root.get());
    Status status = StringToResult(printed, result);
    return status.ok() ? Result<std::string>::Success(std::move(result))
                       : Result<std::string>::Failure(status.code, status.message);
}

Result<std::string> LinxJsonCodec::EncodeListenStop(const voice::VoiceSessionConfig& config) const {
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "listen");
    cJSON_AddStringToObject(root.get(), "state", "stop");
    // Linx defines mode on listen.start only. Keep stop to the documented
    // shape used by the reference SparkBot client.
    if (!config.session_id.empty()) {
        cJSON_AddStringToObject(root.get(), "session_id", config.session_id.c_str());
    }
    std::string result;
    char* printed = PrintUnformatted(root.get());
    Status status = StringToResult(printed, result);
    return status.ok() ? Result<std::string>::Success(std::move(result))
                       : Result<std::string>::Failure(status.code, status.message);
}

Result<std::string> LinxJsonCodec::EncodeListenDetect(const voice::VoiceSessionConfig& config, std::string_view text,
                                                      std::string_view text_response) const {
    if (text.empty()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx detect 文本不能为空");
    }
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "listen");
    cJSON_AddStringToObject(root.get(), "state", "detect");
    cJSON_AddStringToObject(root.get(), "text", std::string(text).c_str());
    if (!text_response.empty()) {
        cJSON_AddStringToObject(root.get(), "text_response", std::string(text_response).c_str());
    }
    if (!config.session_id.empty()) {
        cJSON_AddStringToObject(root.get(), "session_id", config.session_id.c_str());
    }
    std::string result;
    char* printed = PrintUnformatted(root.get());
    Status status = StringToResult(printed, result);
    return status.ok() ? Result<std::string>::Success(std::move(result))
                       : Result<std::string>::Failure(status.code, status.message);
}

Result<std::string> LinxJsonCodec::EncodeAbort(const voice::VoiceSessionConfig& config, std::string_view reason) const {
    if (reason.empty()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx abort 原因不能为空");
    }
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "abort");
    cJSON_AddStringToObject(root.get(), "reason", std::string(reason).c_str());
    if (!config.session_id.empty()) {
        cJSON_AddStringToObject(root.get(), "session_id", config.session_id.c_str());
    }
    std::string result;
    char* printed = PrintUnformatted(root.get());
    Status status = StringToResult(printed, result);
    return status.ok() ? Result<std::string>::Success(std::move(result))
                       : Result<std::string>::Failure(status.code, status.message);
}

// ---- Decode: parse inbound JSON with cJSON_Parse ----

Result<LinxInboundMessage> LinxJsonCodec::DecodeText(std::string_view message) const {
    JsonPtr root(cJSON_ParseWithLength(message.data(), message.size()));
    if (root == nullptr) {
        return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "Linx JSON 解析失败");
    }
    if (!cJSON_IsObject(root.get())) {
        return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "Linx 控制消息必须是 JSON 对象");
    }
    std::string error;
    const cJSON* type = GetRequired(root.get(), "type", cJSON_String, error);
    if (type == nullptr) {
        return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
    }
    const std::string type_str = cJSON_GetStringValue(type);
    LinxInboundMessage decoded;

    // Optional session_id
    const cJSON* sid = GetOptional(root.get(), "session_id", cJSON_String);
    if (sid != nullptr) {
        decoded.session_id = cJSON_GetStringValue(sid);
    }

    if (type_str == "hello") {
        decoded.kind = LinxMessageKind::kHello;
        const cJSON* transport = GetOptional(root.get(), "transport", cJSON_String);
        if (transport != nullptr) {
            const std::string t = cJSON_GetStringValue(transport);
            if (t != "websocket") {
                return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument,
                                                           "Linx hello transport 无效: " + t);
            }
        }
        const cJSON* audio = GetOptional(root.get(), "audio_params", cJSON_Object);
        if (audio != nullptr) {
            auto params = ParseAudioParams(audio);
            if (!params.ok()) {
                return Result<LinxInboundMessage>::Failure(params.status.code, params.status.message);
            }
            decoded.audio_params = *params.value;
        }
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    if (type_str == "stt") {
        decoded.kind = LinxMessageKind::kStt;
        const cJSON* text = GetRequired(root.get(), "text", cJSON_String, error);
        if (text == nullptr) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        decoded.text = cJSON_GetStringValue(text);
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    if (type_str == "tts") {
        decoded.kind = LinxMessageKind::kTts;
        const cJSON* state = GetRequired(root.get(), "state", cJSON_String, error);
        if (state == nullptr) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        const std::string state_str = cJSON_GetStringValue(state);
        if (state_str == "start") {
            decoded.tts_state = LinxTtsState::kStart;
        } else if (state_str == "sentence_start") {
            decoded.tts_state = LinxTtsState::kSentenceStart;
            const cJSON* text = GetOptional(root.get(), "text", cJSON_String);
            if (text != nullptr) decoded.text = cJSON_GetStringValue(text);
        } else if (state_str == "stop") {
            decoded.tts_state = LinxTtsState::kStop;
            const cJSON* aborted = GetOptional(root.get(), "is_aborted", cJSON_False | cJSON_True);
            if (aborted != nullptr) decoded.aborted = cJSON_IsTrue(aborted);
        } else {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "未知 Linx TTS 状态: " + state_str);
        }
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    if (type_str == "mcp") {
        const cJSON* payload = GetRequired(root.get(), "payload", cJSON_Object, error);
        if (payload == nullptr) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        char* serialized = cJSON_PrintUnformatted(payload);
        if (serialized == nullptr) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInternal, "Linx MCP payload 序列化失败");
        }
        decoded.kind = LinxMessageKind::kMcp;
        decoded.text.assign(serialized);
        cJSON_free(serialized);
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    if (type_str == "error") {
        decoded.kind = LinxMessageKind::kError;
        const cJSON* msg = GetOptional(root.get(), "message", cJSON_String);
        if (msg != nullptr) decoded.text = cJSON_GetStringValue(msg);
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    if (type_str == "goodbye") {
        decoded.kind = LinxMessageKind::kGoodbye;
        const cJSON* msg = GetOptional(root.get(), "message", cJSON_String);
        if (msg != nullptr) decoded.text = cJSON_GetStringValue(msg);
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    if (type_str == "llm") {
        decoded.kind = LinxMessageKind::kLlm;
        const cJSON* text = GetOptional(root.get(), "text", cJSON_String);
        if (text != nullptr) decoded.text = cJSON_GetStringValue(text);
        const cJSON* emotion = GetOptional(root.get(), "emotion", cJSON_String);
        if (emotion != nullptr) decoded.emotion = cJSON_GetStringValue(emotion);
        const cJSON* action = GetOptional(root.get(), "action", cJSON_String);
        if (action != nullptr) decoded.action = cJSON_GetStringValue(action);
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }

    return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "未知 Linx 消息类型: " + type_str);
}

}  // namespace voicelife::linx
