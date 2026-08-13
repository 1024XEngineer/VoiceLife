#include "voicelife/mcp/json_rpc_endpoint.h"

#include <iomanip>
#include <sstream>
#include <string>

#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"

namespace voicelife::mcp {
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

std::string IdText(const JsonValue& id) { return id.kind == JsonValue::Kind::kString ? id.string : Serialize(id); }

Result<std::string> ErrorResponse(const JsonValue& id, int code, std::string_view message) {
    return Result<std::string>::Success("{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(id) + ",\"error\":{\"code\":" +
                                        std::to_string(code) + ",\"message\":\"" + Escape(message) + "\"}}");
}

Result<std::string> NullErrorResponse(int code, std::string_view message) {
    return Result<std::string>::Success("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":" + std::to_string(code) +
                                        ",\"message\":\"" + Escape(message) + "\"}}");
}

Result<ToolValue> ToolValueFromJson(const JsonValue& value) {
    if (value.kind == JsonValue::Kind::kString) return Result<ToolValue>::Success(value.string);
    if (value.kind == JsonValue::Kind::kBool) return Result<ToolValue>::Success(value.boolean);
    if (value.kind == JsonValue::Kind::kNumber && value.number == static_cast<int64_t>(value.number)) {
        return Result<ToolValue>::Success(static_cast<int64_t>(value.number));
    }
    return Result<ToolValue>::Failure(ErrorCode::kInvalidArgument, "MCP 工具参数只支持字符串、整数和布尔值");
}

}  // namespace

Result<std::string> JsonRpcEndpoint::Handle(std::string_view request_text) const {
    JsonValue request;
    const Status parsed = ParseJson(request_text, request);
    if (!parsed.ok() || !request.IsObject()) {
        return NullErrorResponse(-32700, "MCP JSON-RPC payload 无效");
    }
    const JsonValue* id = Get(request, "id");
    const JsonValue* method = Get(request, "method");
    if (method == nullptr || !method->IsString()) {
        return id == nullptr ? NullErrorResponse(-32600, "MCP 请求缺少 method")
                             : ErrorResponse(*id, -32600, "MCP 请求缺少 method");
    }
    if (id == nullptr) {
        // JSON-RPC notification 无论方法名为何都不返回响应；MCP 的
        // notifications/initialized 正是标准初始化后通知。
        return Result<std::string>::Success({});
    }
    if (method->string == "initialize") {
        return Result<std::string>::Success("{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(*id) +
                                            ",\"result\":{\"protocolVersion\":\"2024-11-05\","
                                            "\"capabilities\":{\"tools\":{}},\"serverInfo\":{"
                                            "\"name\":\"VoiceLife\",\"version\":\"mvp\"}}}");
    }
    if (method->string == "tools/list") {
        return Result<std::string>::Success("{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(*id) +
                                            ",\"result\":" + server_.list_tools_json() + "}");
    }
    if (method->string != "tools/call") return ErrorResponse(*id, -32601, "未知 MCP 方法");

    const JsonValue* params = Get(request, "params");
    const JsonValue* name = params == nullptr ? nullptr : Get(*params, "name");
    const JsonValue* arguments = params == nullptr ? nullptr : Get(*params, "arguments");
    if (name == nullptr || !name->IsString() || (arguments != nullptr && !arguments->IsObject())) {
        return ErrorResponse(*id, -32602, "tools/call 参数无效");
    }
    ToolArguments converted;
    if (arguments != nullptr) {
        for (const auto& [key, value] : arguments->object) {
            auto converted_value = ToolValueFromJson(value);
            if (!converted_value.ok() || !converted_value.value.has_value()) {
                return ErrorResponse(*id, -32602, converted_value.status.message);
            }
            converted[key] = *converted_value.value;
        }
    }
    const ToolResult call =
        server_.call({.request_id = IdText(*id), .name = name->string, .arguments = std::move(converted)});
    if (!call.status.ok()) {
        return ErrorResponse(*id, call.status.code == ErrorCode::kNotFound ? -32601 : -32602, call.status.message);
    }
    std::string text;
    for (const auto& [key, value] : call.output) {
        if (!text.empty()) text += "\n";
        text += key + "=" + value;
    }
    return Result<std::string>::Success("{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(*id) +
                                        ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + Escape(text) +
                                        "\"}],\"isError\":false}}");
}

Result<std::string> JsonRpcEndpoint::UnavailableResponse(std::string_view request_text, std::string_view message) {
    JsonValue request;
    const Status parsed = ParseJson(request_text, request);
    if (!parsed.ok() || !request.IsObject()) return NullErrorResponse(-32700, "MCP JSON-RPC payload 无效");
    const JsonValue* id = Get(request, "id");
    if (id == nullptr) return Result<std::string>::Success({});
    return ErrorResponse(*id, -32001, message);
}

}  // namespace voicelife::mcp
