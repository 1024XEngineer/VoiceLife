#include "mcp_json_writer.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "voicelife/contracts/tool.h"
#include "yyjson.h"

namespace voicelife::mcp {
namespace {

/** 释放 yyJSON 可变文档。 */
struct MutableDocumentDeleter {
    /**
     * @brief 释放 yyJSON 可变文档。
     * @param document 待释放的文档。
     * @return 无。
     */
    void operator()(yyjson_mut_doc* document) const { yyjson_mut_doc_free(document); }
};

/** 释放 yyJSON 默认分配器生成的字符串。 */
struct JsonStringDeleter {
    /**
     * @brief 释放 yyJSON 写出的字符串。
     * @param text 待释放的字符串。
     * @return 无。
     */
    void operator()(char* text) const { std::free(text); }
};

using MutableDocumentPtr = std::unique_ptr<yyjson_mut_doc, MutableDocumentDeleter>;
using JsonStringPtr = std::unique_ptr<char, JsonStringDeleter>;

/**
 * @brief 创建引用调用方字符串的 yyJSON 字符串节点。
 * @param document 节点所属文档。
 * @param value 字符串内容，必须存活到文档写出完成。
 * @return 创建成功时返回节点，否则返回 nullptr。
 */
yyjson_mut_val* MakeString(yyjson_mut_doc* document, std::string_view value) {
    return yyjson_mut_strn(document, value.data(), value.size());
}

/**
 * @brief 向 JSON 对象添加字符串成员。
 * @param document 节点所属文档。
 * @param object 目标对象。
 * @param key 成员名称。
 * @param value 成员值。
 * @return 添加成功时返回 true，否则返回 false。
 */
bool AddString(yyjson_mut_doc* document, yyjson_mut_val* object, std::string_view key, std::string_view value) {
    return yyjson_mut_obj_add(object, MakeString(document, key), MakeString(document, value));
}

/**
 * @brief 向 JSON 对象添加有符号整数成员。
 * @param document 节点所属文档。
 * @param object 目标对象。
 * @param key 成员名称。
 * @param value 成员值。
 * @return 添加成功时返回 true，否则返回 false。
 */
bool AddInteger(yyjson_mut_doc* document, yyjson_mut_val* object, std::string_view key, int64_t value) {
    return yyjson_mut_obj_add(object, MakeString(document, key), yyjson_mut_sint(document, value));
}

/**
 * @brief 向 JSON 对象添加空对象成员。
 * @param document 节点所属文档。
 * @param object 目标对象。
 * @param key 成员名称。
 * @return 添加成功时返回新对象，否则返回 nullptr。
 */
yyjson_mut_val* AddObject(yyjson_mut_doc* document, yyjson_mut_val* object, std::string_view key) {
    yyjson_mut_val* value = yyjson_mut_obj(document);
    return yyjson_mut_obj_add(object, MakeString(document, key), value) ? value : nullptr;
}

/**
 * @brief 向 JSON 对象添加空数组成员。
 * @param document 节点所属文档。
 * @param object 目标对象。
 * @param key 成员名称。
 * @return 添加成功时返回新数组，否则返回 nullptr。
 */
yyjson_mut_val* AddArray(yyjson_mut_doc* document, yyjson_mut_val* object, std::string_view key) {
    yyjson_mut_val* value = yyjson_mut_arr(document);
    return yyjson_mut_obj_add(object, MakeString(document, key), value) ? value : nullptr;
}

/**
 * @brief 将结构化工具输出值写入 mutable yyjson 文档。
 * @param document 节点所属文档。
 * @param output 待写入的工具输出。
 * @return 创建成功时返回节点，否则返回 nullptr。
 */
yyjson_mut_val* BuildToolOutputValue(yyjson_mut_doc* document, const ToolOutputValue& output) {
    switch (output.kind) {
        case ToolOutputValue::Kind::kNull:
            return yyjson_mut_null(document);
        case ToolOutputValue::Kind::kBoolean:
            return yyjson_mut_bool(document, output.boolean);
        case ToolOutputValue::Kind::kInteger:
            return yyjson_mut_sint(document, output.integer);
        case ToolOutputValue::Kind::kString:
            return MakeString(document, output.string);
        case ToolOutputValue::Kind::kArray: {
            yyjson_mut_val* array = yyjson_mut_arr(document);
            if (array == nullptr) return nullptr;
            if (output.array != nullptr) {
                for (const auto& item : *output.array) {
                    yyjson_mut_val* child =
                        item == nullptr ? yyjson_mut_null(document) : BuildToolOutputValue(document, *item);
                    if (child == nullptr || !yyjson_mut_arr_append(array, child)) return nullptr;
                }
            }
            return array;
        }
        case ToolOutputValue::Kind::kObject: {
            yyjson_mut_val* object = yyjson_mut_obj(document);
            if (object == nullptr) return nullptr;
            if (output.object != nullptr) {
                for (const auto& [key, value] : *output.object) {
                    yyjson_mut_val* child =
                        value == nullptr ? yyjson_mut_null(document) : BuildToolOutputValue(document, *value);
                    if (child == nullptr || !yyjson_mut_obj_add(object, MakeString(document, key), child))
                        return nullptr;
                }
            }
            return object;
        }
    }
    return nullptr;
}

/**
 * @brief 将 mutable yyjson 文档写出为字符串。
 * @param document 已设置根节点的文档。
 * @return 序列化结果。
 */
std::string WriteDocument(yyjson_mut_doc* document) {
    if (document == nullptr) return "{}";
    size_t length = 0;
    JsonStringPtr text(yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &length));
    return text == nullptr ? "{}" : std::string(text.get(), length);
}

/**
 * @brief 获取工具输入类型对应的 JSON Schema 类型名称。
 * @param type 工具输入类型。
 * @return JSON Schema 类型名称。
 */
std::string_view InputTypeName(ToolInputType type) {
    switch (type) {
        case ToolInputType::kBoolean:
            return "boolean";
        case ToolInputType::kInteger:
            return "integer";
        case ToolInputType::kString:
            return "string";
        case ToolInputType::kObject:
            return "object";
    }
    return "string";
}

/**
 * @brief 将单个输入字段追加到所属对象。
 * @param document 节点所属文档。
 * @param object 所属对象。
 * @param name 字段名。
 * @param field 输入字段定义。
 * @return 追加成功时返回 true，否则返回 false。
 */
bool AppendInputField(yyjson_mut_doc* document, yyjson_mut_val* object, std::string_view name,
                      const ToolInputField& field) {
    yyjson_mut_val* property = AddObject(document, object, name);
    if (property == nullptr || !AddString(document, property, "type", InputTypeName(field.type)) ||
        (!field.description.empty() && !AddString(document, property, "description", field.description)) ||
        (field.minimum.has_value() && !AddInteger(document, property, "minimum", *field.minimum)) ||
        (field.maximum.has_value() && !AddInteger(document, property, "maximum", *field.maximum))) {
        return false;
    }
    if ((field.min_length.has_value() && !AddInteger(document, property, "minLength", *field.min_length)) ||
        (field.max_length.has_value() && !AddInteger(document, property, "maxLength", *field.max_length))) {
        return false;
    }
    if (field.type == ToolInputType::kObject && field.object_schema != nullptr) {
        yyjson_mut_val* properties = AddObject(document, property, "properties");
        if (properties == nullptr) return false;
        for (const auto& [child_name, child_field] : field.object_schema->properties) {
            if (!AppendInputField(document, properties, child_name, child_field)) return false;
        }
        if (!field.object_schema->required.empty()) {
            yyjson_mut_val* required = AddArray(document, property, "required");
            if (required == nullptr) return false;
            for (const auto& child_name : field.object_schema->required) {
                if (!yyjson_mut_arr_append(required, MakeString(document, child_name))) return false;
            }
        }
    }
    return true;
}

/**
 * @brief 将单个工具定义追加到工具数组。
 * @param document 节点所属文档。
 * @param tools 目标工具数组。
 * @param definition 工具定义。
 * @return 追加成功时返回 true，否则返回 false。
 */
bool AppendTool(yyjson_mut_doc* document, yyjson_mut_val* tools, const ToolDefinition& definition) {
    yyjson_mut_val* tool = yyjson_mut_obj(document);
    if (tool == nullptr || !AddString(document, tool, "name", definition.name) ||
        !AddString(document, tool, "description", definition.description)) {
        return false;
    }
    (void)AddInteger(document, tool, "type", 0);

    yyjson_mut_val* schema = AddObject(document, tool, "inputSchema");
    if (schema == nullptr || !AddString(document, schema, "type", definition.input_schema.type)) {
        return false;
    }

    yyjson_mut_val* properties = AddObject(document, schema, "properties");
    if (properties == nullptr) {
        return false;
    }
    for (const auto& [name, field] : definition.input_schema.properties) {
        if (!AppendInputField(document, properties, name, field)) {
            return false;
        }
    }

    yyjson_mut_val* required = AddArray(document, schema, "required");
    if (required == nullptr) {
        return false;
    }
    for (const auto& name : definition.input_schema.required) {
        if (!yyjson_mut_arr_append(required, MakeString(document, name))) {
            return false;
        }
    }
    return yyjson_mut_arr_append(tools, tool);
}

}  // namespace

std::string SerializeListToolsResultPage(const ListToolsResult& result, std::size_t begin_index,
                                         std::size_t end_index) {
    if (begin_index > end_index || end_index > result.tools.size()) return "{}";

    MutableDocumentPtr document(yyjson_mut_doc_new(nullptr));
    if (!document) {
        return "{}";
    }

    yyjson_mut_val* root = yyjson_mut_obj(document.get());
    if (root == nullptr) {
        return "{}";
    }
    yyjson_mut_doc_set_root(document.get(), root);

    yyjson_mut_val* tools = AddArray(document.get(), root, "tools");
    if (tools == nullptr) {
        return "{}";
    }
    for (std::size_t index = begin_index; index < end_index; ++index) {
        if (!AppendTool(document.get(), tools, result.tools[index])) {
            return "{}";
        }
    }

    const std::string next_cursor_text = std::to_string(end_index);
    yyjson_mut_val* next_cursor = end_index == result.tools.size() ? yyjson_mut_null(document.get())
                                                                   : MakeString(document.get(), next_cursor_text);
    if (next_cursor == nullptr || !yyjson_mut_obj_add(root, MakeString(document.get(), "nextCursor"), next_cursor)) {
        return "{}";
    }

    return WriteDocument(document.get());
}

std::string SerializeListToolsResult(const ListToolsResult& result) {
    return SerializeListToolsResultPage(result, 0, result.tools.size());
}

std::string SerializeToolOutputValue(const ToolOutputValue& output) {
    MutableDocumentPtr document(yyjson_mut_doc_new(nullptr));
    if (!document) return "{}";

    yyjson_mut_val* root = BuildToolOutputValue(document.get(), output);
    if (root == nullptr) return "{}";
    yyjson_mut_doc_set_root(document.get(), root);
    return WriteDocument(document.get());
}

}  // namespace voicelife::mcp
