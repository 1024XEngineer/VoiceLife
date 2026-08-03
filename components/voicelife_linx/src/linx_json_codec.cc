#include "voicelife/linx/linx_types.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>

namespace voicelife::linx {
namespace {

void SkipSpace(std::string_view text, std::size_t& position) {
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
}

bool ReadJsonString(std::string_view text, std::size_t position, std::string& value,
                   std::size_t* next_position = nullptr) {
    SkipSpace(text, position);
    if (position >= text.size() || text[position] != '"') {
        return false;
    }
    ++position;
    value.clear();
    while (position < text.size()) {
        const char character = text[position++];
        if (character == '"') {
            if (next_position != nullptr) {
                *next_position = position;
            }
            return true;
        }
        if (static_cast<unsigned char>(character) < 0x20U) {
            return false;
        }
        if (character != '\\' || position >= text.size()) {
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            return false;
        }
        const char escaped = text[position++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                // The ESP implementation will use cJSON for full unicode
                // handling. Rejecting \u escapes here keeps this portable
                // fixture codec deterministic instead of corrupting text.
                return false;
        }
    }
    return false;
}

bool FindField(std::string_view object, std::string_view key, std::size_t& value_position) {
    std::size_t position = 0;
    while (position < object.size()) {
        if (object[position] != '"') {
            ++position;
            continue;
        }
        std::string candidate;
        std::size_t after_key = 0;
        if (!ReadJsonString(object, position, candidate, &after_key)) {
            return false;
        }
        position = after_key;
        if (candidate != key) {
            continue;
        }
        SkipSpace(object, position);
        if (position >= object.size() || object[position] != ':') {
            return false;
        }
        value_position = position + 1;
        return true;
    }
    return false;
}

bool ReadStringField(std::string_view object, std::string_view key, std::string& value, bool required,
                     std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少字符串字段: " + std::string(key);
            return false;
        }
        return true;
    }
    if (!ReadJsonString(object, position, value)) {
        error = "字符串字段格式无效: " + std::string(key);
        return false;
    }
    return true;
}

bool ReadUnsignedField(std::string_view object, std::string_view key, uint32_t& value, bool required,
                       std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少数字字段: " + std::string(key);
            return false;
        }
        return true;
    }
    SkipSpace(object, position);
    const std::size_t begin = position;
    uint64_t parsed = 0;
    while (position < object.size() && std::isdigit(static_cast<unsigned char>(object[position])) != 0) {
        const uint32_t digit = static_cast<uint32_t>(object[position] - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
            error = "数字字段超出范围: " + std::string(key);
            return false;
        }
        parsed = parsed * 10U + digit;
        ++position;
    }
    if (position == begin) {
        error = "数字字段格式无效: " + std::string(key);
        return false;
    }
    SkipSpace(object, position);
    if (position < object.size() && object[position] != ',' && object[position] != '}') {
        error = "数字字段尾部无效: " + std::string(key);
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ReadBoolField(std::string_view object, std::string_view key, bool& value, bool required,
                   std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少布尔字段: " + std::string(key);
            return false;
        }
        return true;
    }
    SkipSpace(object, position);
    if (object.substr(position, 4) == "true") {
        value = true;
        position += 4;
        SkipSpace(object, position);
        if (position < object.size() && object[position] != ',' && object[position] != '}') {
            error = "布尔字段尾部无效: " + std::string(key);
            return false;
        }
        return true;
    }
    if (object.substr(position, 5) == "false") {
        value = false;
        position += 5;
        SkipSpace(object, position);
        if (position < object.size() && object[position] != ',' && object[position] != '}') {
            error = "布尔字段尾部无效: " + std::string(key);
            return false;
        }
        return true;
    }
    error = "布尔字段格式无效: " + std::string(key);
    return false;
}

bool ReadObjectField(std::string_view object, std::string_view key, std::string_view& value,
                    bool required, std::string& error) {
    std::size_t position = 0;
    if (!FindField(object, key, position)) {
        if (required) {
            error = "缺少对象字段: " + std::string(key);
            return false;
        }
        return true;
    }
    SkipSpace(object, position);
    if (position >= object.size() || object[position] != '{') {
        error = "对象字段格式无效: " + std::string(key);
        return false;
    }
    const std::size_t begin = position++;
    int depth = 1;
    bool in_string = false;
    bool escaped = false;
    while (position < object.size()) {
        const char character = object[position++];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}' && --depth == 0) {
            value = object.substr(begin, position - begin);
            return true;
        }
    }
    error = "对象字段未闭合: " + std::string(key);
    return false;
}

std::string Quote(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result.push_back('"');
    for (const char character : text) {
        switch (character) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result.push_back(character);
                break;
        }
    }
    result.push_back('"');
    return result;
}

const char* CodecName(voice::AudioCodec codec) {
    return codec == voice::AudioCodec::kOpus ? "opus" : "pcm";
}

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

Result<LinxAudioParams> ParseAudioParams(std::string_view object) {
    std::string error;
    LinxAudioParams params;
    std::string format;
    uint32_t sample_rate = params.sample_rate_hz;
    uint32_t channels = params.channels;
    uint32_t bits = params.bits_per_sample;
    uint32_t duration = params.frame_duration_ms;
    if (!ReadStringField(object, "format", format, true, error) ||
        !ReadUnsignedField(object, "sample_rate", sample_rate, true, error) ||
        !ReadUnsignedField(object, "channels", channels, true, error) ||
        !ReadUnsignedField(object, "bit_depth", bits, false, error) ||
        !ReadUnsignedField(object, "frame_duration", duration, false, error)) {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, error);
    }
    if (format == "pcm") {
        params.codec = voice::AudioCodec::kPcmS16Le;
    } else if (format == "opus") {
        params.codec = voice::AudioCodec::kOpus;
    } else {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, "不支持的 Linx 音频格式");
    }
    if (sample_rate > 0xFFFFFFFFU || channels > 0xFFU || bits > 0xFFU || duration > 0xFFFFU ||
        sample_rate == 0 || channels == 0 || bits == 0 || duration == 0) {
        return Result<LinxAudioParams>::Failure(ErrorCode::kInvalidArgument, "Linx 音频参数超出范围");
    }
    params.sample_rate_hz = sample_rate;
    params.channels = static_cast<uint8_t>(channels);
    params.bits_per_sample = static_cast<uint8_t>(bits);
    params.frame_duration_ms = static_cast<uint16_t>(duration);
    return Result<LinxAudioParams>::Success(params);
}

}  // namespace

Result<std::string> LinxJsonCodec::EncodeHello(const voice::VoiceSessionConfig& config,
                                               const LinxConnectionConfig& connection) const {
    (void)connection;
    if (!config.audio.valid()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx hello 音频参数无效");
    }
    const uint32_t frame_size = config.audio.sample_rate_hz * config.audio.frame_duration_ms / 1000U;
    std::ostringstream json;
    json << "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
         << "\"audio_params\":{\"format\":" << Quote(CodecName(config.audio.codec))
         << ",\"sample_rate\":" << config.audio.sample_rate_hz
         << ",\"channels\":" << static_cast<unsigned>(config.audio.channels)
         << ",\"bit_depth\":" << static_cast<unsigned>(config.audio.bits_per_sample)
         << ",\"endianness\":\"little\",\"frame_duration\":" << config.audio.frame_duration_ms
         << ",\"frame_size\":" << frame_size
         << ",\"sample_format\":\"signed_int16\",\"play_buffer_duration\":1000}}";
    return Result<std::string>::Success(json.str());
}

Result<std::string> LinxJsonCodec::EncodeListenStart(const voice::VoiceSessionConfig& config) const {
    std::ostringstream json;
    json << "{\"type\":\"listen\",\"state\":\"start\",\"mode\":"
         << Quote(ModeName(config.mode));
    if (!config.session_id.empty()) {
        json << ",\"session_id\":" << Quote(config.session_id);
    }
    json << '}';
    return Result<std::string>::Success(json.str());
}

Result<std::string> LinxJsonCodec::EncodeListenStop(const voice::VoiceSessionConfig& config) const {
    std::ostringstream json;
    json << "{\"type\":\"listen\",\"state\":\"stop\",\"mode\":"
         << Quote(ModeName(config.mode));
    if (!config.session_id.empty()) {
        json << ",\"session_id\":" << Quote(config.session_id);
    }
    json << '}';
    return Result<std::string>::Success(json.str());
}

Result<std::string> LinxJsonCodec::EncodeListenDetect(const voice::VoiceSessionConfig& config,
                                                      std::string_view text) const {
    if (text.empty()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx detect 文本不能为空");
    }
    std::ostringstream json;
    json << "{\"type\":\"listen\",\"state\":\"detect\",\"text\":" << Quote(text);
    if (!config.session_id.empty()) {
        json << ",\"session_id\":" << Quote(config.session_id);
    }
    json << '}';
    return Result<std::string>::Success(json.str());
}

Result<std::string> LinxJsonCodec::EncodeAbort(const voice::VoiceSessionConfig& config,
                                               std::string_view reason) const {
    if (reason.empty()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx abort 原因不能为空");
    }
    std::ostringstream json;
    json << "{\"type\":\"abort\",\"reason\":" << Quote(reason);
    if (!config.session_id.empty()) {
        json << ",\"session_id\":" << Quote(config.session_id);
    }
    json << '}';
    return Result<std::string>::Success(json.str());
}

Result<LinxInboundMessage> LinxJsonCodec::DecodeText(std::string_view message) const {
    std::size_t first = 0;
    SkipSpace(message, first);
    std::size_t last = message.size();
    while (last > first && std::isspace(static_cast<unsigned char>(message[last - 1])) != 0) {
        --last;
    }
    if (first >= last || message[first] != '{' || message[last - 1] != '}') {
        return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "Linx 控制消息必须是 JSON 对象");
    }
    std::string error;
    std::string type;
    if (!ReadStringField(message, "type", type, true, error)) {
        return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
    }
    LinxInboundMessage decoded;
    std::string session_id;
    if (!ReadStringField(message, "session_id", session_id, false, error)) {
        return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
    }
    if (!session_id.empty()) {
        decoded.session_id = std::move(session_id);
    }
    if (type == "hello") {
        decoded.kind = LinxMessageKind::kHello;
        std::string transport;
        if (!ReadStringField(message, "transport", transport, false, error) ||
            (!transport.empty() && transport != "websocket")) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "Linx hello transport 无效");
        }
        std::string_view audio_object;
        if (!ReadObjectField(message, "audio_params", audio_object, false, error)) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        if (!audio_object.empty()) {
            auto params = ParseAudioParams(audio_object);
            if (!params.ok()) {
                return Result<LinxInboundMessage>::Failure(params.status.code, params.status.message);
            }
            decoded.audio_params = *params.value;
        }
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }
    if (type == "stt") {
        decoded.kind = LinxMessageKind::kStt;
        if (!ReadStringField(message, "text", decoded.text, true, error)) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }
    if (type == "tts") {
        decoded.kind = LinxMessageKind::kTts;
        std::string state;
        if (!ReadStringField(message, "state", state, true, error)) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        if (state == "start") {
            decoded.tts_state = LinxTtsState::kStart;
        } else if (state == "sentence_start") {
            decoded.tts_state = LinxTtsState::kSentenceStart;
            if (!ReadStringField(message, "text", decoded.text, false, error)) {
                return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
            }
        } else if (state == "stop") {
            decoded.tts_state = LinxTtsState::kStop;
            if (!ReadBoolField(message, "is_aborted", decoded.aborted, false, error)) {
                return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
            }
        } else {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "未知 Linx TTS 状态");
        }
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }
    if (type == "error") {
        decoded.kind = LinxMessageKind::kError;
        if (!ReadStringField(message, "message", decoded.text, false, error)) {
            return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, error);
        }
        return Result<LinxInboundMessage>::Success(std::move(decoded));
    }
    return Result<LinxInboundMessage>::Failure(ErrorCode::kInvalidArgument, "未知 Linx 消息类型: " + type);
}

}  // namespace voicelife::linx
