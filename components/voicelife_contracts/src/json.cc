#include "voicelife/contracts/json.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

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

Status JsonResourceError(const char* message) { return Status::Error(ErrorCode::kUnavailable, message); }

struct YyJsonDeleter {
    void operator()(yyjson_doc* value) const { yyjson_doc_free(value); }
};

using YyJsonDocument = std::unique_ptr<yyjson_doc, YyJsonDeleter>;

struct AllocationBudget {
    size_t used = 0;
    size_t limit = 0;
};

void* Allocate(size_t size) {
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
#else
    return std::malloc(size);
#endif
}

void* Reallocate(void* ptr, size_t size) {
#if defined(ESP_PLATFORM)
    return heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
#else
    return std::realloc(ptr, size);
#endif
}

void Deallocate(void* ptr) {
#if defined(ESP_PLATFORM)
    heap_caps_free(ptr);
#else
    std::free(ptr);
#endif
}

void* BoundedAllocate(void* context, size_t size) {
    auto& budget = *static_cast<AllocationBudget*>(context);
    if (size > budget.limit - std::min(budget.used, budget.limit)) {
        return nullptr;
    }
    void* ptr = Allocate(size);
    if (ptr != nullptr) {
        budget.used += size;
    }
    return ptr;
}

void* BoundedReallocate(void* context, void* ptr, size_t old_size, size_t size) {
    auto& budget = *static_cast<AllocationBudget*>(context);
    if (ptr == nullptr) {
        return BoundedAllocate(context, size);
    }
    if (size == 0) {
        Deallocate(ptr);
        budget.used -= std::min(budget.used, old_size);
        return nullptr;
    }
    if (size > old_size && size - old_size > budget.limit - std::min(budget.used, budget.limit)) {
        return nullptr;
    }
    void* replacement = Reallocate(ptr, size);
    if (replacement != nullptr) {
        if (size >= old_size) {
            budget.used += size - old_size;
        } else {
            budget.used -= std::min(budget.used, old_size - size);
        }
    }
    return replacement;
}

void BoundedDeallocate(void*, void* ptr) { Deallocate(ptr); }

struct ConversionBudget {
    const JsonParseOptions& options;
    size_t nodes = 0;
};

Status ConvertValue(yyjson_val* source, JsonValue& out, ConversionBudget& budget, size_t depth) {
    if (depth > budget.options.max_depth) {
        return InvalidJson("JSON nesting depth exceeds limit");
    }
    if (++budget.nodes > budget.options.max_nodes) {
        return InvalidJson("JSON node count exceeds limit");
    }
    if (unsafe_yyjson_is_null(source)) {
        out = JsonValue{};
        return Status::Ok();
    }
    if (unsafe_yyjson_is_bool(source)) {
        out = JsonValue::Bool(unsafe_yyjson_get_bool(source));
        return Status::Ok();
    }
    if (unsafe_yyjson_is_num(source)) {
        out = JsonValue::Number(unsafe_yyjson_get_num(source));
        return Status::Ok();
    }
    if (unsafe_yyjson_is_str(source)) {
        if (unsafe_yyjson_get_len(source) > budget.options.max_string_bytes) {
            return InvalidJson("JSON string exceeds limit");
        }
        out = JsonValue::String(std::string(unsafe_yyjson_get_str(source), unsafe_yyjson_get_len(source)));
        return Status::Ok();
    }
    if (unsafe_yyjson_is_arr(source)) {
        if (unsafe_yyjson_get_len(source) > budget.options.max_array_items) {
            return InvalidJson("JSON array item count exceeds limit");
        }
        std::vector<JsonValue> values;
        values.reserve(unsafe_yyjson_get_len(source));
        size_t index = 0;
        size_t count = 0;
        yyjson_val* child = nullptr;
        yyjson_arr_foreach(source, index, count, child) {
            JsonValue value;
            if (const Status status = ConvertValue(child, value, budget, depth + 1); !status.ok()) {
                return status;
            }
            values.push_back(std::move(value));
        }
        out = JsonValue::Array(std::move(values));
        return Status::Ok();
    }

    JsonValue::ObjectMap values;
    size_t index = 0;
    size_t count = 0;
    yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
    if (unsafe_yyjson_get_len(source) > budget.options.max_object_members) {
        return InvalidJson("JSON object member count exceeds limit");
    }
    yyjson_obj_foreach(source, index, count, key, value) {
        if (unsafe_yyjson_get_len(key) > budget.options.max_string_bytes) {
            return InvalidJson("JSON object key exceeds limit");
        }
        JsonValue converted;
        if (const Status status = ConvertValue(value, converted, budget, depth + 1); !status.ok()) {
            return status;
        }
        values.insert_or_assign(std::string(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key)),
                                std::move(converted));
    }
    out = JsonValue::Object(std::move(values));
    return Status::Ok();
}

}  // namespace

Status ParseJson(std::string_view input, JsonValue& out, const JsonParseOptions& options) {
    if (input.size() > options.max_bytes) {
        return InvalidJson("JSON document exceeds byte limit");
    }
    AllocationBudget allocation_budget{0, options.max_allocator_bytes};
    const yyjson_alc allocator{
        .malloc = BoundedAllocate,
        .realloc = BoundedReallocate,
        .free = BoundedDeallocate,
        .ctx = &allocation_budget,
    };
    yyjson_read_err error{};
    YyJsonDocument document(
        yyjson_read_opts(const_cast<char*>(input.data()), input.size(), YYJSON_READ_NOFLAG, &allocator, &error));
    if (!document) {
        if (error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
            return JsonResourceError("JSON allocator budget exceeded");
        }
        return InvalidJson(error.msg == nullptr ? "Invalid JSON document" : error.msg);
    }

    JsonValue parsed;
    ConversionBudget conversion_budget{options};
    if (const Status status = ConvertValue(yyjson_doc_get_root(document.get()), parsed, conversion_budget, 0);
        !status.ok()) {
        return status;
    }
    out = std::move(parsed);
    return Status::Ok();
}

}  // namespace voicelife
