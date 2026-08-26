#include "voicelife/mcp/mcp_server.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "mcp_json_writer.h"

namespace voicelife::mcp {
namespace {

/**
 * @brief 判断工具参数值是否符合声明类型。
 * @param value 待检查的参数值。
 * @param type 参数声明类型。
 * @return 类型匹配时返回 true，否则返回 false。
 */
bool MatchesType(const ToolValue& value, ToolInputType type) {
    switch (type) {
        case ToolInputType::kBoolean:
            return std::holds_alternative<bool>(value);
        case ToolInputType::kInteger:
            return std::holds_alternative<int64_t>(value);
        case ToolInputType::kString:
            return std::holds_alternative<std::string>(value);
        case ToolInputType::kObject:
            return std::holds_alternative<JsonValue>(value) && std::get<JsonValue>(value).IsObject();
    }
    return false;
}

/**
 * @brief 创建不包含输出数据的失败结果。
 * @param status 失败状态。
 * @return 工具调用失败结果。
 */
ToolResult Failure(Status status) { return ToolResult::Failure(std::move(status)); }

/**
 * @brief 将业务参数类型转换为 MCP 输入类型。
 * @param type 业务参数类型。
 * @return 对应的 MCP 输入类型。
 */
ToolInputType ToInputType(PropertyType type) {
    switch (type) {
        case PropertyType::kBoolean:
            return ToolInputType::kBoolean;
        case PropertyType::kInteger:
            return ToolInputType::kInteger;
        case PropertyType::kString:
            return ToolInputType::kString;
        case PropertyType::kObject:
            return ToolInputType::kObject;
    }
    return ToolInputType::kString;
}

/**
 * @brief 计算 UTF-8 字符串的字符数量。
 * @param value 待统计的 UTF-8 字符串。
 * @return 不单独计算延续字节的字符数量。
 */
std::size_t Utf8Length(const std::string& value) {
    return static_cast<std::size_t>(
        std::count_if(value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
}

JsonValue ToolValueToJson(const ToolValue& value) {
    if (std::holds_alternative<bool>(value)) return JsonValue::Bool(std::get<bool>(value));
    if (std::holds_alternative<int64_t>(value)) return JsonValue::Number(static_cast<double>(std::get<int64_t>(value)));
    if (std::holds_alternative<std::string>(value)) return JsonValue::String(std::get<std::string>(value));
    if (std::holds_alternative<JsonValue>(value)) return std::get<JsonValue>(value);
    return JsonValue{};
}

bool IsRequired(const ToolInputSchema& schema, const std::string& name) {
    return std::find(schema.required.begin(), schema.required.end(), name) != schema.required.end();
}

Status NormalizeAndValidateObject(const JsonValue& value, const ToolInputSchema& schema, const std::string& path,
                                  JsonValue& normalized) {
    if (!value.IsObject()) return Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + path);
    JsonValue::ObjectMap object;
    std::unordered_set<std::string> defined_names;
    for (const auto& [name, field] : schema.properties) {
        defined_names.insert(name);
        const std::string child_path = path.empty() ? name : path + "." + name;
        const auto argument = value.object.find(name);
        if (argument == value.object.end()) {
            if (field.default_value.has_value()) {
                if (field.type == ToolInputType::kObject && field.object_schema != nullptr) {
                    JsonValue child;
                    const Status status = NormalizeAndValidateObject(ToolValueToJson(*field.default_value),
                                                                     *field.object_schema, child_path, child);
                    if (!status.ok()) return status;
                    object.emplace(name, std::move(child));
                    continue;
                }
                object.emplace(name, ToolValueToJson(*field.default_value));
                continue;
            }
            if (IsRequired(schema, name)) {
                return Status::Error(ErrorCode::kInvalidArgument, "缺少参数：" + child_path);
            }
            continue;
        }
        if (field.type == ToolInputType::kObject && field.object_schema != nullptr) {
            JsonValue child;
            if (const Status status =
                    NormalizeAndValidateObject(argument->second, *field.object_schema, child_path, child);
                !status.ok()) {
                return status;
            }
            object.emplace(name, std::move(child));
            continue;
        }
        if (field.type == ToolInputType::kInteger) {
            if (argument->second.kind != JsonValue::Kind::kNumber ||
                argument->second.number != static_cast<int64_t>(argument->second.number)) {
                return Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + child_path);
            }
            const int64_t number = static_cast<int64_t>(argument->second.number);
            if ((field.minimum.has_value() && number < *field.minimum) ||
                (field.maximum.has_value() && number > *field.maximum)) {
                return Status::Error(ErrorCode::kInvalidArgument, "工具整数参数超出范围：" + child_path);
            }
            object.emplace(name, JsonValue::Number(argument->second.number));
            continue;
        }
        if (field.type == ToolInputType::kString) {
            if (argument->second.kind != JsonValue::Kind::kString) {
                return Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + child_path);
            }
            const std::size_t length = Utf8Length(argument->second.string);
            if ((field.min_length.has_value() && length < *field.min_length) ||
                (field.max_length.has_value() && length > *field.max_length)) {
                return Status::Error(ErrorCode::kInvalidArgument, "工具字符串参数长度超出范围：" + child_path);
            }
            object.emplace(name, argument->second);
            continue;
        }
        if (field.type == ToolInputType::kBoolean) {
            if (argument->second.kind != JsonValue::Kind::kBool) {
                return Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + child_path);
            }
            object.emplace(name, argument->second);
            continue;
        }
        object.emplace(name, argument->second);
    }
    for (const auto& [name, member] : value.object) {
        (void)member;
        if (!defined_names.contains(name)) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "不支持的参数：" + (path.empty() ? name : path + "." + name));
        }
    }
    normalized = JsonValue::Object(std::move(object));
    return Status::Ok();
}

Status ValidatePropertyDefinition(const Property& property, const std::string& path) {
    const ToolInputType input_type = ToInputType(property.type());
    bool default_string_length_invalid = false;
    if (property.default_value().has_value() && input_type == ToolInputType::kString &&
        std::holds_alternative<std::string>(*property.default_value())) {
        const std::size_t length = Utf8Length(std::get<std::string>(*property.default_value()));
        default_string_length_invalid = (property.min_length().has_value() && length < *property.min_length()) ||
                                        (property.max_length().has_value() && length > *property.max_length());
    }
    bool default_integer_range_invalid = false;
    if (property.default_value().has_value() && input_type == ToolInputType::kInteger &&
        std::holds_alternative<int64_t>(*property.default_value())) {
        const int64_t value = std::get<int64_t>(*property.default_value());
        default_integer_range_invalid = (property.minimum().has_value() && value < *property.minimum()) ||
                                        (property.maximum().has_value() && value > *property.maximum());
    }
    if ((property.default_value().has_value() && !MatchesType(*property.default_value(), input_type)) ||
        !property.constraint_valid() || default_string_length_invalid || default_integer_range_invalid ||
        ((property.minimum().has_value() || property.maximum().has_value()) &&
         property.type() != PropertyType::kInteger) ||
        ((property.min_length().has_value() || property.max_length().has_value()) &&
         property.type() != PropertyType::kString) ||
        (property.minimum().has_value() && property.maximum().has_value() &&
         *property.minimum() > *property.maximum()) ||
        (property.min_length().has_value() && property.max_length().has_value() &&
         *property.min_length() > *property.max_length())) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具参数定义无效：" + path);
    }
    if (property.object_properties() != nullptr) {
        if (property.type() != PropertyType::kObject) {
            return Status::Error(ErrorCode::kInvalidArgument, "只有对象参数可以定义内部字段：" + path);
        }
        for (const auto& child : *property.object_properties()) {
            const Status status =
                ValidatePropertyDefinition(child, path.empty() ? child.name() : path + "." + child.name());
            if (!status.ok()) return status;
        }
    }
    return Status::Ok();
}

}  // namespace

Property::Property(std::string name, PropertyType type) : name_(std::move(name)), type_(type) {}

Property::Property(std::string name, PropertyType type, ToolValue default_value)
    : name_(std::move(name)),
      type_(type),
      default_value_(std::move(default_value)),
      required_(!default_value_.has_value()) {}

Property::Property(std::string name, PropertyList object_properties)
    : name_(std::move(name)),
      type_(PropertyType::kObject),
      object_properties_(std::make_shared<PropertyList>(std::move(object_properties))) {}

Property::Property(std::string name, PropertyType type, int64_t minimum, int64_t maximum,
                   std::optional<ToolValue> default_value)
    : name_(std::move(name)),
      type_(type),
      default_value_(std::move(default_value)),
      required_(!default_value_.has_value()) {
    switch (type_) {
        case PropertyType::kInteger:
            minimum_ = minimum;
            maximum_ = maximum;
            break;
        case PropertyType::kString:
            if (minimum < 0 || maximum < 0 ||
                static_cast<std::uintmax_t>(minimum) > std::numeric_limits<std::size_t>::max() ||
                static_cast<std::uintmax_t>(maximum) > std::numeric_limits<std::size_t>::max()) {
                constraint_valid_ = false;
                break;
            }
            min_length_ = static_cast<std::size_t>(minimum);
            max_length_ = static_cast<std::size_t>(maximum);
            break;
        case PropertyType::kBoolean:
        case PropertyType::kObject:
            constraint_valid_ = false;
            break;
    }
}

Property& Property::with_description(std::string description) {
    description_ = std::move(description);
    return *this;
}

Property& Property::with_object_properties(PropertyList object_properties) {
    object_properties_ = std::make_shared<PropertyList>(std::move(object_properties));
    return *this;
}

Property::~Property() = default;

Property Property::WithIntegerRange(std::string name, int64_t minimum, int64_t maximum,
                                    std::optional<ToolValue> default_value) {
    Property property(std::move(name), PropertyType::kInteger);
    property.default_value_ = std::move(default_value);
    property.minimum_ = minimum;
    property.maximum_ = maximum;
    property.required_ = !property.default_value_.has_value();
    return property;
}

Property Property::Optional(std::string name, PropertyType type) {
    Property property(std::move(name), type);
    property.required_ = false;
    return property;
}

Property Property::OptionalObject(std::string name, PropertyList object_properties) {
    Property property(std::move(name), std::move(object_properties));
    property.required_ = false;
    return property;
}

void PropertyList::add_property(Property property) { properties_.push_back(std::move(property)); }

ToolInputSchema PropertyList::to_schema() const {
    ToolInputSchema schema;
    for (const auto& property : properties_) {
        ToolInputField field{.type = ToInputType(property.type()),
                             .default_value = property.default_value(),
                             .description = property.description(),
                             .object_schema = nullptr,
                             .minimum = property.minimum(),
                             .maximum = property.maximum(),
                             .min_length = property.min_length(),
                             .max_length = property.max_length()};
        if (property.object_properties() != nullptr) {
            field.object_schema = std::make_shared<ToolInputSchema>(property.object_properties()->to_schema());
        }
        schema.properties.emplace(property.name(), std::move(field));
        if (property.required() && !property.default_value().has_value()) {
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

std::string McpServer::list_tools_json() const { return SerializeListToolsResult(list_tools()); }

Result<std::string> McpServer::list_tools_page_json(std::size_t start_index, std::size_t maximum_json_bytes) const {
    const ListToolsResult result = list_tools();
    if (start_index > result.tools.size()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "tools/list cursor 超出范围");
    }

    if (start_index == result.tools.size()) {
        const std::string terminal = SerializeListToolsResultPage(result, start_index, start_index);
        if (terminal != "{}" && terminal.size() <= maximum_json_bytes) {
            return Result<std::string>::Success(terminal);
        }
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "tools/list 响应超过安全上限");
    }

    std::string selected;
    for (std::size_t end_index = start_index + 1; end_index <= result.tools.size(); ++end_index) {
        std::string candidate = SerializeListToolsResultPage(result, start_index, end_index);
        if (candidate == "{}" || candidate.size() > maximum_json_bytes) continue;
        selected = std::move(candidate);
    }
    if (selected.empty()) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "单个工具定义超过安全上限");
    }
    return Result<std::string>::Success(std::move(selected));
}

PropertyList PropertyList::with_values(const ToolArguments& arguments) const {
    PropertyList result = *this;
    result.values_ = arguments;
    return result;
}

Status McpServer::add_tool(std::string name, std::string description, PropertyList properties,
                           PropertyHandler handler) {
    if (!handler) return Status::Error(ErrorCode::kInvalidArgument, "工具回调为空");
    auto property_handler = std::move(handler);
    const PropertyList property_schema = properties;
    return add_tool_with_context(
        std::move(name), std::move(description), std::move(properties),
        [property_handler = std::move(property_handler), property_schema](const ToolCall& call) {
            return property_handler(property_schema.with_values(call.arguments));
        });
}

Status McpServer::add_tool_with_context(std::string name, std::string description, PropertyList properties,
                                        ToolHandler handler) {
    // 工具定义完整性校验
    if (name.empty() || description.empty() || !handler) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具定义不完整");
    }

    // 工具名称不能重复注册
    if (tools_.contains(name)) {
        return Status::Error(ErrorCode::kAlreadyExists, "工具已注册：" + name);
    }

    // 校验参数默认值、类型及取值约束，包括对象内部字段。
    for (const auto& property : properties) {
        if (const Status status = ValidatePropertyDefinition(property, property.name()); !status.ok()) {
            return status;
        }
    }

    // 保存工具定义；带上下文回调自行读取已经校验过的 ToolCall 参数。
    const std::string registered_name = name;
    tools_.emplace(registered_name, RegisteredTool{.definition = {.name = std::move(name),
                                                                  .description = std::move(description),
                                                                  .input_schema = properties.to_schema()},
                                                   .handler = std::move(handler)});
    registration_order_.push_back(registered_name);
    return Status::Ok();
}

ToolResult McpServer::call(const ToolCall& call) const {
    // 校验调用标识并查找已注册工具
    if (call.request_id.empty()) {
        return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具调用缺少 request_id"));
    }
    const auto registered = tools_.find(call.name);
    if (registered == tools_.end()) {
        return Failure(Status::Error(ErrorCode::kNotFound, "工具不存在：" + call.name));
    }

    // 按工具声明补充默认值，并校验必填参数和参数类型
    ToolCall normalized_call = call;
    std::unordered_set<std::string> defined_names;
    for (const auto& [name, field] : registered->second.definition.input_schema.properties) {
        defined_names.insert(name);
        const auto argument = call.arguments.find(name);
        if (argument == call.arguments.end()) {
            if (field.default_value.has_value()) {
                if (field.type == ToolInputType::kObject && field.object_schema != nullptr) {
                    JsonValue normalized;
                    const Status status = NormalizeAndValidateObject(std::get<JsonValue>(*field.default_value),
                                                                     *field.object_schema, name, normalized);
                    if (!status.ok()) return Failure(status);
                    normalized_call.arguments.emplace(name, std::move(normalized));
                } else {
                    normalized_call.arguments.emplace(name, *field.default_value);
                }
                continue;
            }
            if (std::find(registered->second.definition.input_schema.required.begin(),
                          registered->second.definition.input_schema.required.end(),
                          name) != registered->second.definition.input_schema.required.end()) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "缺少参数：" + name));
            }
        } else if (field.type == ToolInputType::kObject && field.object_schema != nullptr) {
            if (!MatchesType(argument->second, field.type)) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + name));
            }
            JsonValue normalized;
            const Status status = NormalizeAndValidateObject(std::get<JsonValue>(argument->second),
                                                             *field.object_schema, name, normalized);
            if (!status.ok()) return Failure(status);
            normalized_call.arguments[name] = std::move(normalized);
        } else if (!MatchesType(argument->second, field.type)) {
            return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具参数类型错误：" + name));
        } else if (field.type == ToolInputType::kInteger) {
            const auto value = std::get<int64_t>(argument->second);
            if ((field.minimum.has_value() && value < *field.minimum) ||
                (field.maximum.has_value() && value > *field.maximum)) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具整数参数超出范围：" + name));
            }
        } else if (field.type == ToolInputType::kString) {
            const auto& value = std::get<std::string>(argument->second);
            const std::size_t length = Utf8Length(value);
            if ((field.min_length.has_value() && length < *field.min_length) ||
                (field.max_length.has_value() && length > *field.max_length)) {
                return Failure(Status::Error(ErrorCode::kInvalidArgument, "工具字符串参数长度超出范围：" + name));
            }
        }
    }

    // 拒绝工具声明之外的未知参数
    for (const auto& [name, value] : call.arguments) {
        (void)value;
        if (!defined_names.contains(name)) {
            return Failure(Status::Error(ErrorCode::kInvalidArgument, "不支持的参数：" + name));
        }
    }

    // 执行业务回调并返回执行结果
    return registered->second.handler(normalized_call);
}

}  // namespace voicelife::mcp
