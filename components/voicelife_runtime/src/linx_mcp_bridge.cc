#include "linx_mcp_bridge.h"

#include <iomanip>
#include <sstream>
#include <string>

#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"

namespace voicelife::runtime {
namespace {

std::string Escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
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
                if (character < 0x20U) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<unsigned>(character);
                    result += escaped.str();
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    return result;
}

std::string Serialize(const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::kNull:
            return "null";
        case JsonValue::Kind::kBool:
            return value.boolean ? "true" : "false";
        case JsonValue::Kind::kNumber: {
            std::ostringstream number;
            number << std::setprecision(17) << value.number;
            return number.str();
        }
        case JsonValue::Kind::kString:
            return "\"" + Escape(value.string) + "\"";
        case JsonValue::Kind::kArray: {
            std::string result = "[";
            for (std::size_t index = 0; index < value.array.size(); ++index) {
                if (index != 0) result += ",";
                result += Serialize(value.array[index]);
            }
            return result + "]";
        }
        case JsonValue::Kind::kObject: {
            std::string result = "{";
            std::size_t index = 0;
            for (const auto& [key, item] : value.object) {
                if (index++ != 0) result += ",";
                result += "\"" + Escape(key) + "\":" + Serialize(item);
            }
            return result + "}";
        }
    }
    return "null";
}

const JsonValue* Get(const JsonValue& object, const char* key) { return object.IsObject() ? object.Get(key) : nullptr; }

std::string IdText(const JsonValue& id) {
    if (id.kind == JsonValue::Kind::kString) return id.string;
    return Serialize(id);
}

std::string Wrap(std::string payload, std::string_view session_id) {
    std::string result = "{\"type\":\"mcp\"";
    if (!session_id.empty()) result += ",\"session_id\":\"" + Escape(session_id) + "\"";
    result += ",\"payload\":" + std::move(payload) + "}";
    return result;
}

Result<std::string> ErrorResponse(const JsonValue& id, int code, std::string_view message,
                                  std::string_view session_id) {
    const std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(id) +
                                ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + Escape(message) +
                                "\"}}";
    return Result<std::string>::Success(Wrap(payload, session_id));
}

Result<ToolValue> ToolValueFromJson(const JsonValue& value) {
    if (value.kind == JsonValue::Kind::kString) return Result<ToolValue>::Success(value.string);
    if (value.kind == JsonValue::Kind::kBool) return Result<ToolValue>::Success(value.boolean);
    if (value.kind == JsonValue::Kind::kNumber && value.number == static_cast<int64_t>(value.number)) {
        return Result<ToolValue>::Success(static_cast<int64_t>(value.number));
    }
    if (value.kind == JsonValue::Kind::kObject) {
        return Result<ToolValue>::Success(value);
    }
    return Result<ToolValue>::Failure(ErrorCode::kInvalidArgument, "MCP 工具参数只支持字符串、整数、布尔值和对象");
}

/**
 * @brief 获取工具调用面向用户的文本结果。
 * @param result 已成功执行的工具结果。
 * @return 工具提供的精确文本，或由结构化输出序列化生成的 JSON 文本。
 */
std::string ResolveToolResultText(const ToolResult& result) {
    if (result.text_output.has_value()) return *result.text_output;
    return mcp::SerializeToolOutputValue(result.output);
}

}  // namespace

Result<std::string> HandleLinxMcpPayload(std::string_view payload, const mcp::McpServer& server,
                                         std::string_view session_id) {
    JsonValue request;
    const Status parsed = ParseJson(payload, request);
    if (!parsed.ok() || !request.IsObject()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "MCP JSON-RPC payload 无效");
    }
    const JsonValue* id = Get(request, "id");
    const JsonValue* method = Get(request, "method");
    if (method == nullptr || !method->IsString()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "MCP 请求缺少 method");
    }
    // JSON-RPC notifications (including MCP's notifications/initialized)
    // intentionally have no id and must not produce a response frame.
    if (id == nullptr) {
        if (method->string.rfind("notifications/", 0) == 0 || method->string == "ping") {
            return Result<std::string>::Success(std::string{});
        }
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "MCP 请求缺少 id");
    }
    if (method->string == "initialize") {
        const std::string result = "{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(*id) +
                                   ",\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
                                   "\"serverInfo\":{\"name\":\"VoiceLife\",\"version\":\"mvp\"}}}";
        return Result<std::string>::Success(Wrap(result, session_id));
    }
    if (method->string == "tools/list") {
        return Result<std::string>::Success(
            Wrap("{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(*id) + ",\"result\":" + server.list_tools_json() + "}",
                 session_id));
    }
    if (method->string != "tools/call") return ErrorResponse(*id, -32601, "未知 MCP 方法", session_id);

    const JsonValue* params = Get(request, "params");
    const JsonValue* name = params == nullptr ? nullptr : Get(*params, "name");
    const JsonValue* arguments = params == nullptr ? nullptr : Get(*params, "arguments");
    if (name == nullptr || !name->IsString() || (arguments != nullptr && !arguments->IsObject())) {
        return ErrorResponse(*id, -32602, "tools/call 参数无效", session_id);
    }
    ToolArguments converted;
    if (arguments != nullptr) {
        for (const auto& [key, value] : arguments->object) {
            auto converted_value = ToolValueFromJson(value);
            if (!converted_value.ok() || !converted_value.value.has_value())
                return ErrorResponse(*id, -32602, converted_value.status.message, session_id);
            converted[key] = *converted_value.value;
        }
    }
    const auto call = server.call({.request_id = IdText(*id), .name = name->string, .arguments = std::move(converted)});
    if (!call.status.ok()) {
        const int code = call.status.code == ErrorCode::kNotFound ? -32601 : -32602;
        return ErrorResponse(*id, code, call.status.message, session_id);
    }
    const std::string text = ResolveToolResultText(call);
    const std::string result = "{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(*id) +
                               ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + Escape(text) +
                               "\"}],\"isError\":false}}";
    return Result<std::string>::Success(Wrap(result, session_id));
}

}  // namespace voicelife::runtime
