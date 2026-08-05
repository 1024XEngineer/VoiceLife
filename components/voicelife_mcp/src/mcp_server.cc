#include "voicelife/mcp/mcp_server.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "cjson/cJSON.h"

namespace voicelife::mcp {
namespace {

bool MatchesType(const ToolValue& value, ToolInputType type) {
    switch (type) {
        case ToolInputType::kBoolean:
            return std::holds_alternative<bool>(value);
        case ToolInputType::kInteger:
            return std::holds_alternative<int64_t>(value);
        case ToolInputType::kString:
            return std::holds_alternative<std::string>(value);
    }
    return false;
}

ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

ToolInputType ToInputType(PropertyType type) {
    switch (type) {
        case PropertyType::kBoolean:
            return ToolInputType::kBoolean;
        case PropertyType::kInteger:
            return ToolInputType::kInteger;
        case PropertyType::kString:
            return ToolInputType::kString;
    }
    return ToolInputType::kString;
}

}  // namespace

Property::Property(std::string name, PropertyType type) : name_(std::move(name)), type_(type) {}

Property::Property(std::string name, PropertyType type, ToolValue default_value)
    : name_(std::move(name)), type_(type), default_value_(std::move(default_value)) {}

Property::Property(std::string name, PropertyType type, int64_t minimum, int64_t maximum)
    : name_(std::move(name)), type_(type), minimum_(minimum), maximum_(maximum) {
    if (type != PropertyType::kInteger || minimum > maximum) {
        throw std::invalid_argument("整数参数范围无效");
    }
}

void PropertyList::add_property(Property property) { properties_.push_back(std::move(property)); }

const Property& PropertyList::operator[](const std::string& name) const {
    for (const auto& property : properties_) {
        if (property.name() == name) {
            return property;
        }
    }
    throw std::out_of_range("工具参数不存在：" + name);
}

ToolInputSchema PropertyList::to_schema() const {
    ToolInputSchema schema;
    for (const auto& property : properties_) {
        ToolInputField field{.type = ToInputType(property.type()),
                             .default_value = property.default_value(),
                             .description = {},
                             .minimum = property.minimum(),
                             .maximum = property.maximum()};
        schema.properties.emplace(property.name(), std::move(field));
        if (!property.default_value().has_value()) {
            schema.required.push_back(property.name());
        }
    }
    return schema;
}

ListToolsResult McpServer::list_tools() const {
    ListToolsResult result;
    result.tools.reserve(registration_order_.size());
    for (const auto& name : registration_order_) {
        result.tools.push_back(tools_.at(name).definition);
    }
    result.total = result.tools.size();
    return result;
}

std::string McpServer::list_tools_json() const {
    cJSON* result = cJSON_CreateObject();
    cJSON* tools = cJSON_AddArrayToObject(result, "tools");
    for (const auto& definition : list_tools().tools) {
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", definition.name.c_str());
        cJSON_AddStringToObject(tool, "description", definition.description.c_str());
        cJSON* schema = cJSON_AddObjectToObject(tool, "inputSchema");
        cJSON_AddStringToObject(schema, "type", definition.input_schema.type.c_str());
        cJSON* properties = cJSON_AddObjectToObject(schema, "properties");
        for (const auto& [name, field] : definition.input_schema.properties) {
            cJSON* property = cJSON_AddObjectToObject(properties, name.c_str());
            const char* type = field.type == ToolInputType::kInteger ? "integer"
                               : field.type == ToolInputType::kBoolean ? "boolean"
                                                                       : "string";
            cJSON_AddStringToObject(property, "type", type);
            if (!field.description.empty()) {
                cJSON_AddStringToObject(property, "description", field.description.c_str());
            }
            if (field.minimum.has_value()) {
                cJSON_AddNumberToObject(property, "minimum", static_cast<double>(*field.minimum));
            }
            if (field.maximum.has_value()) {
                cJSON_AddNumberToObject(property, "maximum", static_cast<double>(*field.maximum));
            }
        }
        cJSON* required = cJSON_AddArrayToObject(schema, "required");
        for (const auto& name : definition.input_schema.required) {
            cJSON_AddItemToArray(required, cJSON_CreateString(name.c_str()));
        }
        cJSON_AddItemToArray(tools, tool);
    }
    char* text = cJSON_Print(result);
    std::string output = text == nullptr ? "{}" : text;
    cJSON_free(text);
    cJSON_Delete(result);
    return output;
}

PropertyList PropertyList::with_values(const ToolArguments& arguments) const {
    PropertyList result = *this;
    result.values_ = arguments;
    return result;
}

Status McpServer::add_tool(std::string name, std::string description, PropertyList properties, PropertyHandler handler) {
    if (name.empty() || description.empty() || !handler) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具定义不完整");
    }
    if (tools_.contains(name)) {
        return Status::Error(ErrorCode::kAlreadyExists, "工具已注册：" + name);
    }
    const std::string registered_name = name;
    tools_.emplace(registered_name,
                   RegisteredTool{.definition = {.name = std::move(name),
                                                 .description = std::move(description),
                                                 .input_schema = properties.to_schema()},
                                  .handler = [properties = std::move(properties), handler = std::move(handler)](
                                                 const ToolCall& call) {
                                      return handler(properties.with_values(call.arguments));
                                  }});
    registration_order_.push_back(registered_name);
    return Status::Ok();
}

ToolResult McpServer::call(const ToolCall& call) const {
    if (call.request_id.empty()) {
        return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具调用缺少 request_id"));
    }
    const auto registered = tools_.find(call.name);
    if (registered == tools_.end()) {
        return Failure(Status::Error(ErrorCode::kNotFound, "工具不存在：" + call.name));
    }
    ToolCall normalized_call = call;
    std::unordered_set<std::string> defined_names;
    for (const auto& [name, field] : registered->second.definition.input_schema.properties) {
        defined_names.insert(name);
        const auto argument = call.arguments.find(name);
        if (argument == call.arguments.end()) {
            if (field.default_value.has_value()) {
                normalized_call.arguments.emplace(name, *field.default_value);
                continue;
            }
            if (std::find(registered->second.definition.input_schema.required.begin(),
                          registered->second.definition.input_schema.required.end(), name) !=
                registered->second.definition.input_schema.required.end()) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "缺少参数：" + name));
            }
        } else if (!MatchesType(argument->second, field.type)) {
            return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + name));
        } else if (field.type == ToolInputType::kInteger) {
            const auto value = std::get<int64_t>(argument->second);
            if ((field.minimum.has_value() && value < *field.minimum) ||
                (field.maximum.has_value() && value > *field.maximum)) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具整数参数超出范围：" + name));
            }
        }
    }
    for (const auto& [name, value] : call.arguments) {
        (void)value;
        if (!defined_names.contains(name)) {
            return Failure(Status::Error(ErrorCode::kInvalidArgument, "不支持的参数：" + name));
        }
    }
    return registered->second.handler(normalized_call);
}

}  // namespace voicelife::mcp
