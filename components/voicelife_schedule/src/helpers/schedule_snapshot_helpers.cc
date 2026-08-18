#include "schedule_snapshot_helpers.h"

#include <string>
#include <string_view>

namespace voicelife::schedule {
namespace {

/** @brief 将时间点转为 Unix 秒整数。 @param time 时间点。 @return 自纪元起的秒数。 */
int64_t ToEpochSeconds(DateTime time) { return time.time_since_epoch().count(); }

/** @brief 追加带引号且转义后的 JSON 字符串字段值。 @param out 输出缓冲。 @param value 原始文本。 @return 无。 */
void AppendJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char character : value) {
        switch (character) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(character);
                break;
        }
    }
    out.push_back('"');
}

/** @brief 追加可选整数字段值（无值时输出 null）。 @param out 输出缓冲。 @param key 字段名。 @param value 可选值。
 * @return 无。 */
void AppendOptionalInteger(std::string& out, std::string_view key, const std::optional<int64_t>& value) {
    out += '"';
    out.append(key);
    out += "\":";
    if (value.has_value()) {
        out += std::to_string(*value);
    } else {
        out += "null";
    }
    out.push_back(',');
}

/** @brief 追加可选字符串字段值（无值时输出 null）。 @param out 输出缓冲。 @param key 字段名。 @param value 可选文本。
 * @return 无。 */
void AppendOptionalString(std::string& out, std::string_view key, const std::optional<std::string>& value) {
    out += '"';
    out.append(key);
    out += "\":";
    if (value.has_value()) {
        AppendJsonString(out, *value);
    } else {
        out += "null";
    }
    out.push_back(',');
}

}  // namespace

std::string SerializeScheduleSnapshot(const Schedule& schedule) {
    std::string out;
    out.reserve(192);
    out.push_back('{');

    out += "\"id\":";
    out += std::to_string(schedule.id);
    out.push_back(',');

    out += "\"event\":";
    AppendJsonString(out, schedule.event);
    out.push_back(',');

    const std::optional<int64_t> start =
        schedule.start_time.has_value() ? std::optional<int64_t>(ToEpochSeconds(*schedule.start_time)) : std::nullopt;
    AppendOptionalInteger(out, "start_time", start);
    const std::optional<int64_t> end =
        schedule.end_time.has_value() ? std::optional<int64_t>(ToEpochSeconds(*schedule.end_time)) : std::nullopt;
    AppendOptionalInteger(out, "end_time", end);
    AppendOptionalString(out, "location", schedule.location);
    AppendOptionalString(out, "notes", schedule.notes);

    const std::optional<int64_t> rule_id =
        schedule.rule_id.has_value() ? std::optional<int64_t>(*schedule.rule_id) : std::nullopt;
    AppendOptionalInteger(out, "rule_id", rule_id);

    out += "\"status\":";
    out += std::to_string(static_cast<int>(schedule.status));
    out.push_back(',');

    out += "\"created_at\":";
    out += std::to_string(ToEpochSeconds(schedule.created_at));
    out.push_back(',');

    out += "\"updated_at\":";
    out += std::to_string(ToEpochSeconds(schedule.updated_at));

    out.push_back('}');
    return out;
}

}  // namespace voicelife::schedule
