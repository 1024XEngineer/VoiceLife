#include "voicelife/mcp/mcp_server.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>

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
    }
    return false;
}

/**
 * @brief 创建不包含输出数据的失败结果。
 * @param status 失败状态。
 * @return 工具调用失败结果。
 */
ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

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

}  // namespace

Property::Property(std::string name, PropertyType type) : name_(std::move(name)), type_(type) {}

Property::Property(std::string name, PropertyType type, ToolValue default_value)
    : name_(std::move(name)),
      type_(type),
      default_value_(std::move(default_value)),
      required_(!default_value_.has_value()) {}

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
            constraint_valid_ = false;
            break;
    }
}

Property Property::Optional(std::string name, PropertyType type) {
    Property property(std::move(name), type);
    property.required_ = false;
    return property;
}

void PropertyList::add_property(Property property) { properties_.push_back(std::move(property)); }

ToolInputSchema PropertyList::to_schema() const {
    ToolInputSchema schema;
    for (const auto& property : properties_) {
        ToolInputField field{.type = ToInputType(property.type()),
                             .default_value = property.default_value(),
                             .description = {},
                             .minimum = property.minimum(),
                             .maximum = property.maximum(),
                             .min_length = property.min_length(),
                             .max_length = property.max_length()};
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

PropertyList PropertyList::with_values(const ToolArguments& arguments) const {
    PropertyList result = *this;
    result.values_ = arguments;
    return result;
}

Status McpServer::add_tool(std::string name, std::string description, PropertyList properties,
                           PropertyHandler handler) {
    // 工具定义完整性校验
    if (name.empty() || description.empty() || !handler) {
        return Status::Error(ErrorCode::kInvalidArgument, "工具定义不完整");
    }

    // 工具名称不能重复注册
    if (tools_.contains(name)) {
        return Status::Error(ErrorCode::kAlreadyExists, "工具已注册：" + name);
    }

    // 校验参数默认值、类型及取值约束
    for (const auto& property : properties) {
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
            return Status::Error(ErrorCode::kInvalidArgument, "工具参数定义无效：" + property.name());
        }
    }

    // 保存工具定义，并在调用时注入已校验的参数
    const std::string registered_name = name;
    tools_.emplace(registered_name, RegisteredTool{.definition = {.name = std::move(name),
                                                                  .description = std::move(description),
                                                                  .input_schema = properties.to_schema()},
                                                   .handler = [properties = std::move(properties),
                                                               handler = std::move(handler)](const ToolCall& call) {
                                                       return handler(properties.with_values(call.arguments));
                                                   }});
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
                normalized_call.arguments.emplace(name, *field.default_value);
                continue;
            }
            if (std::find(registered->second.definition.input_schema.required.begin(),
                          registered->second.definition.input_schema.required.end(),
                          name) != registered->second.definition.input_schema.required.end()) {
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
