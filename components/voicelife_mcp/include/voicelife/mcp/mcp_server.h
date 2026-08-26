#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "voicelife/contracts/tool.h"

namespace voicelife::mcp {

/// MCP 工具参数支持的数据类型。
enum class ToolInputType { kString, kInteger, kBoolean, kObject };

/// MCP 工具输入参数的 JSON Schema 前向声明。
struct ToolInputSchema;

/// MCP 工具的单个输入字段定义。
struct ToolInputField {
    ToolInputType type = ToolInputType::kString;
    std::optional<ToolValue> default_value;
    std::string description;
    std::shared_ptr<ToolInputSchema> object_schema;
    std::optional<int64_t> minimum;
    std::optional<int64_t> maximum;
    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
};

/// MCP 工具输入参数的 JSON Schema。
struct ToolInputSchema {
    std::string type = "object";
    std::unordered_map<std::string, ToolInputField> properties;
    std::vector<std::string> required;
};

/// MCP 工具对外公开的定义，不包含本地执行回调。
struct ToolDefinition {
    std::string name;
    std::string description;
    ToolInputSchema input_schema;
};

/// 模型发起工具调用时执行的本地回调。
using ToolHandler = std::function<ToolResult(const ToolCall&)>;

/// MCP 工具列表及其总数。
struct ListToolsResult {
    std::vector<ToolDefinition> tools;
    std::size_t total = 0;
};

/// 工具参数支持的类型。
enum class PropertyType { kBoolean, kInteger, kString, kObject };

/// 面向业务代码的 MCP 参数集合前向声明。
class PropertyList;

/// 面向业务代码的单个工具参数声明。
class Property {
   public:
    /**
     * @brief 创建没有默认值的参数声明。
     * @param name 参数名称。
     * @param type 参数类型。
     * @return 无。
     */
    Property(std::string name, PropertyType type);
    /**
     * @brief 创建带默认值的参数声明。
     * @param name 参数名称。
     * @param type 参数类型。
     * @param default_value 默认值。
     * @return 无。
     */
    Property(std::string name, PropertyType type, ToolValue default_value);
    /**
     * @brief 创建带内部字段定义的对象参数声明。
     * @param name 参数名称。
     * @param object_properties 对象内部字段定义。
     * @return 无。
     */
    Property(std::string name, PropertyList object_properties);
    /**
     * @brief 创建带数值或字符串长度约束的参数声明。
     * @param name 参数名称。
     * @param type 参数类型；整数类型使用数值范围，字符串类型使用字符长度。
     * @param minimum 最小值或最小字符数。
     * @param maximum 最大值或最大字符数。
     * @param default_value 默认值；未设置时该参数为必填。
     * @return 无。
     */
    Property(std::string name, PropertyType type, int64_t minimum, int64_t maximum,
             std::optional<ToolValue> default_value = std::nullopt);

    /**
     * @brief 设置参数字段描述。
     * @param description 输出到 JSON Schema 字段上的描述。
     * @return 当前参数声明，便于链式构造。
     */
    Property& with_description(std::string description);
    /**
     * @brief 设置对象参数内部字段定义。
     * @param object_properties 对象内部字段定义。
     * @return 当前参数声明，便于链式构造。
     */
    Property& with_object_properties(PropertyList object_properties);

    /** @brief 释放参数声明占用的资源。 */
    ~Property();

    /**
     * @brief 创建带整数范围约束的参数声明。
     * @param name 参数名称。
     * @param minimum 最小值（含）。
     * @param maximum 最大值（含）。
     * @param default_value 默认值；未设置时该参数为必填。
     * @return 参数声明。
     */
    static Property WithIntegerRange(std::string name, int64_t minimum, int64_t maximum,
                                     std::optional<ToolValue> default_value = std::nullopt);

    /**
     * @brief 创建一个没有默认值但允许调用方省略的参数声明。
     * @param name 参数名称。
     * @param type 参数类型。
     * @return 可选参数声明。
     */
    static Property Optional(std::string name, PropertyType type);
    /**
     * @brief 创建可省略的对象参数声明并设置内部字段定义。
     * @param name 参数名称。
     * @param object_properties 对象内部字段定义。
     * @return 可选对象参数声明。
     */
    static Property OptionalObject(std::string name, PropertyList object_properties);

    /**
     * @brief 获取参数名称。
     * @return 参数名称。
     */
    [[nodiscard]] const std::string& name() const { return name_; }
    /**
     * @brief 获取参数类型。
     * @return 参数类型。
     */
    [[nodiscard]] PropertyType type() const { return type_; }
    /**
     * @brief 获取参数字段描述。
     * @return 字段描述；未设置时为空字符串。
     */
    [[nodiscard]] const std::string& description() const { return description_; }
    /**
     * @brief 获取对象参数内部字段定义。
     * @return 内部字段定义；未设置时为空。
     */
    [[nodiscard]] const std::shared_ptr<PropertyList>& object_properties() const { return object_properties_; }
    /**
     * @brief 获取参数默认值。
     * @return 默认值；未设置时为空。
     */
    [[nodiscard]] const std::optional<ToolValue>& default_value() const { return default_value_; }
    /**
     * @brief 获取整数参数最小值。
     * @return 最小值；未设置时为空。
     */
    [[nodiscard]] std::optional<int64_t> minimum() const { return minimum_; }
    /**
     * @brief 获取整数参数最大值。
     * @return 最大值；未设置时为空。
     */
    [[nodiscard]] std::optional<int64_t> maximum() const { return maximum_; }
    /** @brief 获取字符串最小长度。 @return 最小长度；未设置时为空。 */
    [[nodiscard]] std::optional<std::size_t> min_length() const { return min_length_; }
    /** @brief 获取字符串最大长度。 @return 最大长度；未设置时为空。 */
    [[nodiscard]] std::optional<std::size_t> max_length() const { return max_length_; }
    /**
     * @brief 判断约束参数是否能按声明类型解释。
     * @return 约束有效时返回 true。
     */
    [[nodiscard]] bool constraint_valid() const { return constraint_valid_; }
    /**
     * @brief 判断参数缺失时是否应拒绝调用。
     * @return 参数必填时返回 true。
     */
    [[nodiscard]] bool required() const { return required_; }

   private:
    std::string name_;
    PropertyType type_;
    std::string description_;
    std::shared_ptr<PropertyList> object_properties_;
    std::optional<ToolValue> default_value_;
    std::optional<int64_t> minimum_;
    std::optional<int64_t> maximum_;
    std::optional<std::size_t> min_length_;
    std::optional<std::size_t> max_length_;
    bool constraint_valid_ = true;
    bool required_ = true;
};

/// 工具参数声明及调用值的集合。
class PropertyList {
   public:
    /**
     * @brief 创建空参数列表。
     * @return 无。
     */
    PropertyList() = default;
    /**
     * @brief 使用参数声明集合创建参数列表。
     * @param properties 参数声明集合。
     * @return 无。
     */
    explicit PropertyList(std::vector<Property> properties) : properties_(std::move(properties)) {}

    /**
     * @brief 向参数列表追加一个参数声明。
     * @param property 参数声明。
     * @return 无。
     */
    void add_property(Property property);
    /**
     * @brief 读取已绑定的参数值。
     * @param name 参数名称。
     * @return 指定类型的参数值；参数不存在或类型不匹配时为空。
     */
    template <typename T>
    [[nodiscard]] std::optional<T> value(const std::string& name) const;
    /**
     * @brief 将业务参数声明转换为 MCP 输入 Schema。
     * @return 输入 Schema。
     */
    [[nodiscard]] ToolInputSchema to_schema() const;
    /**
     * @brief 将调用参数绑定到参数列表。
     * @param arguments 工具调用参数。
     * @return 带有参数值的参数列表。
     */
    [[nodiscard]] PropertyList with_values(const ToolArguments& arguments) const;

    /**
     * @brief 获取参数声明集合的起始迭代器。
     * @return 起始迭代器。
     */
    auto begin() const { return properties_.begin(); }
    /**
     * @brief 获取参数声明集合的结束迭代器。
     * @return 结束迭代器。
     */
    auto end() const { return properties_.end(); }

   private:
    std::vector<Property> properties_;
    ToolArguments values_;
};

template <typename T>
std::optional<T> PropertyList::value(const std::string& name) const {
    const auto value = values_.find(name);
    if (value == values_.end()) {
        return std::nullopt;
    }
    const auto* typed_value = std::get_if<T>(&value->second);
    return typed_value == nullptr ? std::nullopt : std::optional<T>(*typed_value);
}

/// 使用已校验参数执行工具业务逻辑的回调。
using PropertyHandler = std::function<ToolResult(const PropertyList&)>;

/// 面向业务层的 MCP 工具注册门面。
class McpServer {
   public:
    /**
     * @brief 创建空的 MCP 工具注册服务。
     * @return 无。
     */
    McpServer() = default;

    /**
     * @brief 注册一个带参数描述和业务回调的工具。
     * @param name 工具名称。
     * @param description 工具描述。
     * @param properties 工具参数声明。
     * @param handler 工具执行回调。
     * @return 注册结果。
     */
    Status add_tool(std::string name, std::string description, PropertyList properties, PropertyHandler handler);

    /**
     * @brief 注册可读取 request_id 的工具回调。
     * @param name 工具名称。
     * @param description 工具描述。
     * @param properties 工具参数声明。
     * @param handler 工具执行回调。
     * @return 注册结果。
     */
    Status add_tool_with_context(std::string name, std::string description, PropertyList properties,
                                 ToolHandler handler);

    /**
     * @brief 返回当前已注册工具的结构化列表。
     * @return 按注册顺序排列的工具列表。
     */
    [[nodiscard]] ListToolsResult list_tools() const;

    /**
     * @brief 生成 MCP tools/list 返回的 JSON-RPC result 内容。
     * @return 包含 tools 数组的 JSON 文本。
     */
    [[nodiscard]] std::string list_tools_json() const;

    /**
     * @brief 生成从指定游标开始且不超过字节预算的一页 MCP tools/list 结果。
     * @param start_index 当前页第一个工具的注册顺序下标。
     * @param maximum_json_bytes 当前页 result JSON 的最大字节数。
     * @return 包含完整工具定义和 nextCursor 的一页 JSON，或受控失败。
     */
    [[nodiscard]] Result<std::string> list_tools_page_json(std::size_t start_index,
                                                           std::size_t maximum_json_bytes) const;

    /**
     * @brief 根据工具名称校验参数并执行对应 handler。
     * @param call 工具调用请求。
     * @return 工具执行结果或参数校验错误。
     */
    ToolResult call(const ToolCall& call) const;

   private:
    /// 注册中心内部保存的工具定义与执行回调。
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
    };

    std::unordered_map<std::string, RegisteredTool> tools_;
    std::vector<std::string> registration_order_;
};

/**
 * @brief 将结构化工具输出序列化为紧凑 JSON 文本。
 * @param output 待序列化的工具输出。
 * @return 序列化成功时返回 JSON 文本，失败时返回空对象。
 */
std::string SerializeToolOutputValue(const ToolOutputValue& output);

}  // namespace voicelife::mcp
