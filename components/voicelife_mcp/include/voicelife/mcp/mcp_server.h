#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "voicelife/contracts/tool.h"

namespace voicelife::mcp {

/// MCP 工具参数支持的数据类型。
enum class ToolInputType { kString, kInteger, kBoolean };

/// MCP 工具的单个输入字段定义。
struct ToolInputField {
    ToolInputType type = ToolInputType::kString;
    std::optional<ToolValue> default_value;
    std::string description;
    std::optional<int64_t> minimum;
    std::optional<int64_t> maximum;
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
enum class PropertyType { kBoolean, kInteger, kString };

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
     * @brief 创建带整数范围约束的参数声明。
     * @param name 参数名称。
     * @param type 参数类型，必须为整数。
     * @param minimum 最小值。
     * @param maximum 最大值。
     * @return 无。
     */
    Property(std::string name, PropertyType type, int64_t minimum, int64_t maximum);

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

   private:
    std::string name_;
    PropertyType type_;
    std::optional<ToolValue> default_value_;
    std::optional<int64_t> minimum_;
    std::optional<int64_t> maximum_;
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
     */
    void add_property(Property property);
    /**
     * @brief 按名称读取参数声明。
     * @param name 参数名称。
     * @return 对应参数声明；不存在时抛出异常。
     */
    [[nodiscard]] const Property& operator[](const std::string& name) const;
    /**
     * @brief 读取已绑定的参数值。
     * @param name 参数名称。
     * @return 指定类型的参数值。
     */
    template <typename T>
    [[nodiscard]] T value(const std::string& name) const {
        return std::get<T>(values_.at(name));
    }
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

}  // namespace voicelife::mcp
