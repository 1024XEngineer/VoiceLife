#include "linx_mcp_bridge.h"

#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"

namespace voicelife::runtime {
namespace {

constexpr std::string_view kBindingToolHandledSummary = "绑定操作已处理";
constexpr std::string_view kBindingToolFailedSummary = "绑定操作失败";

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

std::string ToolOutcomeSummary(std::string_view request_payload, bool success) {
    JsonValue request;
    if (!ParseJson(request_payload, request).ok() || !request.IsObject()) {
        return success ? "操作已完成" : "操作失败";
    }
    const JsonValue* params = Get(request, "params");
    const JsonValue* name = params == nullptr ? nullptr : Get(*params, "name");
    if (name == nullptr || !name->IsString()) return success ? "操作已完成" : "操作失败";
    if (name->string == "schedule.create") return success ? "日程已创建" : "日程创建失败";
    if (name->string == "schedule.query") return success ? "日程查询完成" : "日程查询失败";
    if (name->string == "im.binding.start") {
        return std::string(success ? kBindingToolHandledSummary : kBindingToolFailedSummary);
    }
    return success ? "操作已完成" : "操作失败";
}

bool IsKnownToolResultStatus(std::string_view value) {
    return value == "success" || value == "conflict" || value == "failure";
}

std::string ToolResultStatus(const ToolResult& result) {
    if (!result.status.ok()) return "failure";
    if (!result.output.IsObject() || result.output.object == nullptr) return "unknown";
    for (const auto& [key, value] : *result.output.object) {
        if (key != "status" || value == nullptr || !value->IsString()) continue;
        return IsKnownToolResultStatus(value->string) ? value->string : "unknown";
    }
    return "unknown";
}

LinxMcpToolOutcome ToolOutcomeFromResult(std::string_view request_payload, const ToolResult& result) {
    LinxMcpToolOutcome outcome;
    outcome.result_status = ToolResultStatus(result);
    outcome.success = outcome.result_status == "success";
    outcome.summary = ToolOutcomeSummary(request_payload, outcome.success);
    return outcome;
}

void SetToolOutcome(LinxMcpToolOutcome* destination, std::string_view request_payload, const ToolResult& result) {
    if (destination != nullptr) *destination = ToolOutcomeFromResult(request_payload, result);
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

Result<std::size_t> ParseToolsListStart(const JsonValue& request) {
    const JsonValue* params = Get(request, "params");
    if (params == nullptr) return Result<std::size_t>::Success(0);
    if (!params->IsObject()) {
        return Result<std::size_t>::Failure(ErrorCode::kInvalidArgument, "tools/list params 无效");
    }
    const JsonValue* cursor = Get(*params, "cursor");
    if (cursor == nullptr || !cursor->IsString() || cursor->string.empty()) {
        return Result<std::size_t>::Failure(ErrorCode::kInvalidArgument, "tools/list cursor 无效");
    }

    std::size_t start = 0;
    for (const unsigned char character : cursor->string) {
        if (character < '0' || character > '9') {
            return Result<std::size_t>::Failure(ErrorCode::kInvalidArgument, "tools/list cursor 无效");
        }
        const std::size_t digit = character - '0';
        if (start > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return Result<std::size_t>::Failure(ErrorCode::kInvalidArgument, "tools/list cursor 超出范围");
        }
        start = start * 10U + digit;
    }
    return Result<std::size_t>::Success(start);
}

Result<std::string> BuildToolsListResponse(const JsonValue& id, const mcp::McpServer& server,
                                           std::string_view session_id, std::size_t start_index) {
    const std::string json_rpc_prefix = "{\"jsonrpc\":\"2.0\",\"id\":" + Serialize(id) + ",\"result\":";
    const std::string empty_result_envelope = Wrap(json_rpc_prefix + "}", session_id);
    if (empty_result_envelope.size() >= kLinxMcpMaxResponseBytes) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "tools/list 响应超过安全上限");
    }

    const std::size_t result_budget = kLinxMcpMaxResponseBytes - empty_result_envelope.size();
    const auto page = server.list_tools_page_json(start_index, result_budget);
    if (!page.ok() || !page.value.has_value()) {
        return Result<std::string>::Failure(page.status.code, page.status.message);
    }

    const std::string response = Wrap(json_rpc_prefix + *page.value + "}", session_id);
    if (response.size() > kLinxMcpMaxResponseBytes) {
        return Result<std::string>::Failure(ErrorCode::kInternal, "tools/list 响应超过安全上限");
    }
    return Result<std::string>::Success(response);
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
                                         std::string_view session_id, LinxMcpToolOutcome* tool_outcome) {
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
        const auto start = ParseToolsListStart(request);
        if (!start.ok() || !start.value.has_value()) {
            return ErrorResponse(*id, -32602, start.status.message, session_id);
        }
        const auto response = BuildToolsListResponse(*id, server, session_id, *start.value);
        if (response.ok()) return response;
        const int error_code = response.status.code == ErrorCode::kInvalidArgument ? -32602 : -32603;
        return ErrorResponse(*id, error_code, response.status.message, session_id);
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
    SetToolOutcome(tool_outcome, payload, call);
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

Result<std::string> BuildLinxMcpUnavailableResponse(std::string_view payload, std::string_view message,
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
    // 通知没有响应帧；它们也不会进入有业务执行的 worker。
    if (id == nullptr && method->string.rfind("notifications/", 0) == 0) {
        return Result<std::string>::Success(std::string{});
    }
    if (id == nullptr) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "MCP 请求缺少 id");
    }
    return ErrorResponse(*id, -32001, message, session_id);
}

LinxMcpToolOutcome InspectLinxMcpToolOutcome(std::string_view request_payload, const Result<std::string>& response) {
    LinxMcpToolOutcome outcome;
    outcome.summary = ToolOutcomeSummary(request_payload, false);
    if (!response.ok() || !response.value.has_value()) return outcome;

    JsonValue envelope;
    if (!ParseJson(*response.value, envelope).ok() || !envelope.IsObject()) return outcome;
    const JsonValue* payload = Get(envelope, "payload");
    if (payload == nullptr || !payload->IsObject()) return outcome;

    if (const JsonValue* error = Get(*payload, "error"); error != nullptr && error->IsObject()) {
        // JSON-RPC/MCP 错误字段属于诊断信息，可能包含参数名、校验规则或
        // Provider 实现细节。它不能成为设备屏幕上的用户可见文本。
        return outcome;
    }

    const JsonValue* result = Get(*payload, "result");
    if (result == nullptr || !result->IsObject()) return outcome;
    const JsonValue* is_error = Get(*result, "isError");
    if (is_error == nullptr || is_error->kind != JsonValue::Kind::kBool || is_error->boolean) {
        // MCP 的 isError 内容同样是服务端诊断，不向 PresentationPort 透传。
        return outcome;
    }

    outcome.success = true;
    outcome.summary = ToolOutcomeSummary(request_payload, true);
    outcome.result_status = "success";
    // 成功内容同样是 MCP 的机器可读回包（例如 event=、status=、count=）。
    // 它只随 JSON-RPC 响应回传给 Linx，不能成为设备底部用户文案。
    return outcome;
}

bool IsBindingMcpToolSummary(std::string_view summary) {
    return summary == kBindingToolHandledSummary || summary == kBindingToolFailedSummary;
}

}  // namespace voicelife::runtime
