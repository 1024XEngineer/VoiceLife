#include <cstdint>
#include <string>

#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/contracts/tool.h"
#include "voicelife/mcp/mcp_server.h"

using voicelife::ErrorCode;
using voicelife::JsonValue;
using voicelife::Status;
using voicelife::ToolOutputValue;
using voicelife::ToolResult;
using voicelife::mcp::McpServer;
using voicelife::mcp::Property;
using voicelife::mcp::PropertyList;
using voicelife::mcp::PropertyType;
using voicelife::test::Check;

namespace {

/** @brief 返回不做任何工作的成功回调。 @param properties 参数列表。 @return 成功结果。 */
ToolResult NoopHandler(const PropertyList& properties) {
    (void)properties;
    return ToolResult::Success(ToolOutputValue::Null());
}

}  // namespace

/**
 * @brief 执行新增的 MCP Server 参数构造与对象默认值覆盖测试。
 * @return 全部断言通过时返回 0。
 */
int main() {
    // 覆盖无默认值构造、带默认值构造和对象字段构造。
    Property required_flag("flag", PropertyType::kBoolean);
    Check(required_flag.required() && required_flag.type() == PropertyType::kBoolean, "普通参数应默认为必填");

    Property default_count("count", PropertyType::kInteger, int64_t{3});
    Check(!default_count.required() && default_count.default_value().has_value(), "带默认值的参数应标记为可选");

    PropertyList object_properties;
    object_properties.add_property(Property("brightness", PropertyType::kInteger, 0, 100));
    Property nested("settings", object_properties);
    Check(nested.type() == PropertyType::kObject && nested.object_properties() != nullptr,
          "对象参数构造应保存内部字段定义");

    // 覆盖带默认值的对象参数：字段缺失时补默认值。
    McpServer server;
    Check(server
              .add_tool("coverage.object", "对象默认值",
                        PropertyList({Property::OptionalObject(
                            "settings", PropertyList({
                                            Property("count", PropertyType::kInteger, int64_t{7}),
                                        }))}),
                        NoopHandler)
              .ok(),
          "带对象默认值的工具应注册成功");
    Check(server
              .call({.request_id = "object-default",
                     .name = "coverage.object",
                     .arguments = {{"settings", JsonValue::Object({})}}})
              .status.ok(),
          "对象参数缺失内部字段时应补默认值");

    // 覆盖带默认值的对象参数：默认对象内部必填字段缺失时正常返回，并覆盖调用路径。
    Check(server
              .add_tool("coverage.object.optional", "可选对象默认",
                        PropertyList({Property::OptionalObject(
                            "settings", PropertyList({
                                            Property("name", PropertyType::kString, std::string("abc")),
                                        }))}),
                        NoopHandler)
              .ok(),
          "带对象字段默认值的工具应注册成功");
    Check(server
              .call({.request_id = "object-default-2",
                     .name = "coverage.object.optional",
                     .arguments = {{"settings", JsonValue::Object({})}}})
              .status.ok(),
          "对象参数应通过默认值补齐路径");

    // 覆盖失败默认值路径：内部字段默认值类型不匹配时应拒绝。
    Check(server
              .add_tool("coverage.object.invalid", "非法对象默认",
                        PropertyList({Property::OptionalObject(
                            "settings", PropertyList({
                                            Property("count", PropertyType::kInteger, int64_t{1}),
                                        }))}),
                        NoopHandler)
              .ok(),
          "非法对象默认值工具应注册成功");
    Check(server.call({.request_id = "object-default-invalid",
                       .name = "coverage.object.invalid",
                       .arguments = {{"settings", JsonValue::Object({{"count", JsonValue::String("bad")}})}}})
                  .status.code == ErrorCode::kInvalidArgument,
          "对象参数内部字段类型错误应被拒绝");

    // 覆盖分页边界、重复注册和不完整定义的显式错误。
    Check(server.list_tools_page_json(999, 1024).status.code == ErrorCode::kInvalidArgument,
          "超出工具列表的游标应被拒绝");
    Check(server.list_tools_page_json(server.list_tools().total, 1).status.code == ErrorCode::kUnavailable,
          "无法容纳终止页时应返回不可用");
    Check(server.add_tool("", "缺少名称", PropertyList{}, NoopHandler).code == ErrorCode::kInvalidArgument,
          "空工具名称应被拒绝");
    Check(server.add_tool("coverage.object", "重复", PropertyList{}, NoopHandler).code == ErrorCode::kAlreadyExists,
          "重复工具名称应被拒绝");

    // 覆盖标量类型、范围、字符串字符数和未知字段校验。
    PropertyList scalar_properties({Property("enabled", PropertyType::kBoolean),
                                    Property("count", PropertyType::kInteger, 1, 3),
                                    Property("title", PropertyType::kString, 1, 2)});
    Check(server.add_tool("coverage.scalar", "标量参数", scalar_properties, NoopHandler).ok(),
          "有效标量工具应注册成功");
    Check(server.call({.request_id = "", .name = "coverage.scalar"}).status.code == ErrorCode::kInvalidArgument,
          "缺少 request_id 应被拒绝");
    Check(server.call({.request_id = "missing-tool", .name = "unknown"}).status.code == ErrorCode::kNotFound,
          "未知工具应被拒绝");
    Check(server.call({.request_id = "missing-argument", .name = "coverage.scalar"}).status.code ==
              ErrorCode::kInvalidArgument,
          "缺少必填参数应被拒绝");
    Check(server.call({.request_id = "wrong-boolean",
                       .name = "coverage.scalar",
                       .arguments = {{"enabled", std::string("true")},
                                     {"count", int64_t{2}},
                                     {"title", std::string("中")}}})
                  .status.code == ErrorCode::kInvalidArgument,
          "布尔参数类型错误应被拒绝");
    Check(server.call({.request_id = "integer-range",
                       .name = "coverage.scalar",
                       .arguments = {{"enabled", true}, {"count", int64_t{4}}, {"title", std::string("中")}}})
                  .status.code == ErrorCode::kInvalidArgument,
          "整数范围外的参数应被拒绝");
    Check(server.call({.request_id = "utf8-length",
                       .name = "coverage.scalar",
                       .arguments = {{"enabled", true}, {"count", int64_t{2}}, {"title", std::string("中文文")}}})
                  .status.code == ErrorCode::kInvalidArgument,
          "UTF-8 字符长度超限应被拒绝");
    Check(server.call({.request_id = "unknown-argument",
                       .name = "coverage.scalar",
                       .arguments = {{"enabled", true},
                                     {"count", int64_t{2}},
                                     {"title", std::string("中")},
                                     {"extra", int64_t{1}}}})
                  .status.code == ErrorCode::kInvalidArgument,
          "未声明参数应被拒绝");

    // 覆盖不合法的默认值与约束定义，确保注册期即可阻断错误 Schema。
    Check(server.add_tool("coverage.invalid-default", "非法默认值",
                          PropertyList({Property("count", PropertyType::kInteger, std::string("bad"))}), NoopHandler)
                  .code == ErrorCode::kInvalidArgument,
          "与声明类型不一致的默认值应被拒绝");
    Check(server.add_tool("coverage.invalid-range", "反向范围",
                          PropertyList({Property("count", PropertyType::kInteger, 3, 1)}), NoopHandler)
                  .code == ErrorCode::kInvalidArgument,
          "反向整数范围应被拒绝");
    return 0;
}
