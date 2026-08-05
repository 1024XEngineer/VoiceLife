#include "voicelife/contracts/json.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "yyjson.h"

namespace voicelife {

JsonValue JsonValue::Bool(bool value) {
    JsonValue result;
    result.kind = Kind::kBool;
    result.boolean = value;
    return result;
}

JsonValue JsonValue::Number(double value) {
    JsonValue result;
    result.kind = Kind::kNumber;
    result.number = value;
    return result;
}

JsonValue JsonValue::String(std::string value) {
    JsonValue result;
    result.kind = Kind::kString;
    result.string = std::move(value);
    return result;
}

JsonValue JsonValue::Array(std::vector<JsonValue> value) {
    JsonValue result;
    result.kind = Kind::kArray;
    result.array = std::move(value);
    return result;
}

JsonValue JsonValue::Object(ObjectMap value) {
    JsonValue result;
    result.kind = Kind::kObject;
    result.object = std::move(value);
    return result;
}

const JsonValue* JsonValue::Get(const std::string& key) const {
    if (!IsObject()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

namespace {

Status InvalidJson(const char* message) { return Status::Error(ErrorCode::kInvalidArgument, message); }

struct YyJsonDeleter {
    void operator()(yyjson_doc* value) const { yyjson_doc_free(value); }
};

using YyJsonDocument = std::unique_ptr<yyjson_doc, YyJsonDeleter>;

Status ConvertValue(yyjson_val* source, JsonValue& out) {
    if (yyjson_is_null(source)) {
        out = JsonValue{};
        return Status::Ok();
    }
    if (yyjson_is_bool(source)) {
        out = JsonValue::Bool(yyjson_get_bool(source));
        return Status::Ok();
    }
    if (yyjson_is_num(source)) {
        out = JsonValue::Number(yyjson_get_num(source));
        return Status::Ok();
    }
    if (yyjson_is_str(source)) {
        const char* value = yyjson_get_str(source);
        if (value == nullptr) {
            return InvalidJson("yyjson returned a string without a value");
        }
        out = JsonValue::String(std::string(value, yyjson_get_len(source)));
        return Status::Ok();
    }
    if (yyjson_is_arr(source)) {
        std::vector<JsonValue> values;
        values.reserve(yyjson_arr_size(source));
        size_t index = 0;
        size_t count = 0;
        yyjson_val* child = nullptr;
        yyjson_arr_foreach(source, index, count, child) {
            JsonValue value;
            if (Status status = ConvertValue(child, value); !status.ok()) {
                return status;
            }
            values.push_back(std::move(value));
        }
        out = JsonValue::Array(std::move(values));
        return Status::Ok();
    }
    if (yyjson_is_obj(source)) {
        JsonValue::ObjectMap values;
        size_t index = 0;
        size_t count = 0;
        yyjson_val* key = nullptr;
        yyjson_val* value = nullptr;
        yyjson_obj_foreach(source, index, count, key, value) {
            const char* key_text = yyjson_get_str(key);
            if (key_text == nullptr) {
                return InvalidJson("yyjson returned an object member without a key");
            }
            JsonValue converted;
            if (Status status = ConvertValue(value, converted); !status.ok()) {
                return status;
            }
            values.insert_or_assign(std::string(key_text, yyjson_get_len(key)), std::move(converted));
        }
        out = JsonValue::Object(std::move(values));
        return Status::Ok();
    }
    return InvalidJson("yyjson returned an unsupported value type");
}

}  // namespace

Status ParseJson(std::string_view input, JsonValue& out) {
    YyJsonDocument document(yyjson_read(input.data(), input.size(), YYJSON_READ_NOFLAG));
    if (!document) {
        return InvalidJson("Invalid JSON document");
    }

    JsonValue parsed;
    if (Status status = ConvertValue(yyjson_doc_get_root(document.get()), parsed); !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife
