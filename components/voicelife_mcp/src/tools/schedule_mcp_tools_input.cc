#include "schedule_mcp_tools_input.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "schedule_tool_output.h"
#include "voicelife/schedule/schedule_rule_commands.h"

namespace voicelife::mcp::schedule_tool_input {
namespace {

std::optional<schedule::Frequency> ParseFrequency(const std::string& text) {
    if (text == "daily") return schedule::Frequency::kDaily;
    if (text == "weekly") return schedule::Frequency::kWeekly;
    if (text == "monthly") return schedule::Frequency::kMonthly;
    if (text == "yearly") return schedule::Frequency::kYearly;
    return std::nullopt;
}

std::optional<schedule::MonthlyMode> ParseMonthlyMode(const std::string& text) {
    if (text == "specific_day") return schedule::MonthlyMode::kSpecificDay;
    if (text == "last_day") return schedule::MonthlyMode::kLastDay;
    return std::nullopt;
}

std::optional<std::string> JsonString(const JsonValue& object, const std::string& key) {
    const JsonValue* value = object.Get(key);
    return value != nullptr && value->IsString() ? std::optional<std::string>{value->string} : std::nullopt;
}

std::optional<int64_t> JsonInteger(const JsonValue& object, const std::string& key) {
    const JsonValue* value = object.Get(key);
    if (value == nullptr || value->kind != JsonValue::Kind::kNumber ||
        value->number != static_cast<int64_t>(value->number)) {
        return std::nullopt;
    }
    return static_cast<int64_t>(value->number);
}

ParsedRepeat ParseFlat(const PropertyList& properties, bool require_anchor) {
    ParsedRepeat parsed;
    const auto frequency = properties.value<std::string>("freq_type");
    if (frequency.has_value()) {
        parsed.freq_type = ParseFrequency(*frequency);
        if (!parsed.freq_type.has_value()) {
            parsed.error = "freq_type 必须是 daily、weekly、monthly 或 yearly；请不要传入其他周期名称";
            return parsed;
        }
    }

    const auto start_time = properties.value<std::string>("start_time");
    if (start_time.has_value()) {
        parsed.start_time = schedule_tool_output::ParseLocalTime(*start_time);
        if (!parsed.start_time.has_value()) {
            parsed.error = "start_time 必须是严格的 HH:mm:ss 格式，例如 08:30:00";
            return parsed;
        }
    }
    const auto end_time = properties.value<std::string>("end_time");
    if (end_time.has_value()) {
        parsed.end_time = schedule_tool_output::ParseLocalTime(*end_time);
        if (!parsed.end_time.has_value()) {
            parsed.error = "end_time 必须是严格的 HH:mm:ss 格式，例如 09:30:00";
            return parsed;
        }
    }
    const auto start_date = properties.value<std::string>("start_date");
    if (start_date.has_value()) {
        parsed.start_date = schedule_tool_output::ParseLocalDate(*start_date);
        if (!parsed.start_date.has_value()) {
            parsed.error = "start_date 必须是严格的 YYYY-MM-DD 格式，例如 2026-08-27";
            return parsed;
        }
    }
    const auto end_date = properties.value<std::string>("end_date");
    if (end_date.has_value()) {
        parsed.end_date = schedule_tool_output::ParseLocalDate(*end_date);
        if (!parsed.end_date.has_value()) {
            parsed.error = "end_date 必须是严格的 YYYY-MM-DD 格式，例如 2026-12-31";
            return parsed;
        }
    }

    const auto interval = properties.value<int64_t>("interval_val");
    if (interval.has_value()) {
        if (*interval < 1 || *interval > std::numeric_limits<int32_t>::max()) {
            parsed.error = "interval_val 必须是 1 到 2147483647 之间的整数";
            return parsed;
        }
        parsed.interval_val = static_cast<int32_t>(*interval);
    }
    const auto weekdays = properties.value<int64_t>("weekdays_mask");
    if (weekdays.has_value()) {
        if (*weekdays < 1 || *weekdays > 127) {
            parsed.error = "weekdays_mask 必须是 1 到 127 之间的整数；仅 weekly 周期需要传入";
            return parsed;
        }
        parsed.weekdays_mask = static_cast<uint8_t>(*weekdays);
    }
    const auto day = properties.value<int64_t>("day_of_month");
    if (day.has_value()) {
        if (*day < 1 || *day > 31) {
            parsed.error = "day_of_month 必须是 1 到 31 之间的整数；按月指定日期时传入";
            return parsed;
        }
        parsed.day_of_month = static_cast<uint8_t>(*day);
    }
    const auto month = properties.value<int64_t>("month_of_year");
    if (month.has_value()) {
        if (*month < 1 || *month > 12) {
            parsed.error = "month_of_year 必须是 1 到 12 之间的整数；yearly 周期需要传入";
            return parsed;
        }
        parsed.month_of_year = static_cast<uint8_t>(*month);
    }
    const auto mode = properties.value<std::string>("monthly_mode");
    if (mode.has_value()) {
        parsed.monthly_mode = ParseMonthlyMode(*mode);
        if (!parsed.monthly_mode.has_value()) {
            parsed.error = "monthly_mode 只能是 specific_day 或 last_day；仅 monthly 周期使用";
            return parsed;
        }
    }
    const auto count = properties.value<int64_t>("occurrence_count");
    if (count.has_value()) {
        if (*count < 1 || *count > std::numeric_limits<int32_t>::max()) {
            parsed.error = "occurrence_count 必须是 1 到 2147483647 之间的整数";
            return parsed;
        }
        parsed.occurrence_count = static_cast<int32_t>(*count);
    }

    if (require_anchor && (!parsed.freq_type.has_value() || !parsed.start_time.has_value() ||
                           !parsed.start_date.has_value())) {
        parsed.error = "创建周期日程时必须传入 freq_type、start_date 和 start_time；这些字段不能省略";
    }
    return parsed;
}

bool IsKnownRepeatField(const std::string& key) {
    static constexpr std::array<std::string_view, 11> kFields = {
        "freq_type",        "interval_val",  "start_date",   "start_time",    "end_time",     "end_date",
        "occurrence_count", "weekdays_mask", "day_of_month", "month_of_year", "monthly_mode",
    };
    for (const auto field : kFields) {
        if (key == field) return true;
    }
    return false;
}

}  // namespace

ParsedRepeat ParseRepeat(const std::optional<JsonValue>& repeat, bool require_anchor) {
    ParsedRepeat parsed;
    if (!repeat.has_value()) return parsed;
    if (!repeat->IsObject()) {
        parsed.error = "repeat 必须是对象";
        return parsed;
    }
    for (const auto& [key, value] : repeat->object) {
        (void)value;
        if (!IsKnownRepeatField(key)) {
            parsed.error = "repeat 包含不支持的字段：" + key + "；不能把未支持的周期语义近似为其他规则";
            return parsed;
        }
    }

    const auto freq_text = JsonString(*repeat, "freq_type");
    parsed.freq_type = freq_text.has_value() ? ParseFrequency(*freq_text) : std::nullopt;
    if (freq_text.has_value() && !parsed.freq_type.has_value()) {
        parsed.error = "repeat.freq_type 必须是 daily、weekly、monthly 或 yearly";
        return parsed;
    }

    const auto start_time_text = JsonString(*repeat, "start_time");
    parsed.start_time =
        start_time_text.has_value() ? schedule_tool_output::ParseLocalTime(*start_time_text) : std::nullopt;
    if (start_time_text.has_value() && !parsed.start_time.has_value()) {
        parsed.error = "repeat.start_time 格式必须是 HH:mm:ss";
        return parsed;
    }

    const auto end_time_text = JsonString(*repeat, "end_time");
    parsed.end_time = end_time_text.has_value() ? schedule_tool_output::ParseLocalTime(*end_time_text) : std::nullopt;
    if (end_time_text.has_value() && !parsed.end_time.has_value()) {
        parsed.error = "repeat.end_time 格式必须是 HH:mm:ss";
        return parsed;
    }

    const auto start_date_text = JsonString(*repeat, "start_date");
    parsed.start_date =
        start_date_text.has_value() ? schedule_tool_output::ParseLocalDate(*start_date_text) : std::nullopt;
    if (start_date_text.has_value() && !parsed.start_date.has_value()) {
        parsed.error = "repeat.start_date 格式必须是 YYYY-MM-DD";
        return parsed;
    }

    const auto end_date_text = JsonString(*repeat, "end_date");
    parsed.end_date = end_date_text.has_value() ? schedule_tool_output::ParseLocalDate(*end_date_text) : std::nullopt;
    if (end_date_text.has_value() && !parsed.end_date.has_value()) {
        parsed.error = "repeat.end_date 格式必须是 YYYY-MM-DD";
        return parsed;
    }

    const auto monthly_mode_text = JsonString(*repeat, "monthly_mode");
    parsed.monthly_mode = monthly_mode_text.has_value() ? ParseMonthlyMode(*monthly_mode_text) : std::nullopt;
    if (monthly_mode_text.has_value() && !parsed.monthly_mode.has_value()) {
        parsed.error = "repeat.monthly_mode 必须是 specific_day 或 last_day";
        return parsed;
    }

    const auto interval = JsonInteger(*repeat, "interval_val");
    if (repeat->Get("interval_val") != nullptr && !interval.has_value()) {
        parsed.error = "repeat.interval_val 必须是整数";
        return parsed;
    }
    if (interval.has_value() && (*interval < 1 || *interval > INT32_MAX)) {
        parsed.error = "repeat.interval_val 必须在 1 到 2147483647 之间";
        return parsed;
    }
    parsed.interval_val = interval.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*interval)} : std::nullopt;

    const auto weekdays = JsonInteger(*repeat, "weekdays_mask");
    if (repeat->Get("weekdays_mask") != nullptr && (!weekdays.has_value() || *weekdays < 1 || *weekdays > 127)) {
        parsed.error = "repeat.weekdays_mask 必须在 1 到 127 之间";
        return parsed;
    }
    parsed.weekdays_mask =
        weekdays.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*weekdays)} : std::nullopt;
    const auto day = JsonInteger(*repeat, "day_of_month");
    if (repeat->Get("day_of_month") != nullptr && (!day.has_value() || *day < 1 || *day > 31)) {
        parsed.error = "repeat.day_of_month 必须在 1 到 31 之间";
        return parsed;
    }
    parsed.day_of_month = day.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*day)} : std::nullopt;
    const auto month = JsonInteger(*repeat, "month_of_year");
    if (repeat->Get("month_of_year") != nullptr && (!month.has_value() || *month < 1 || *month > 12)) {
        parsed.error = "repeat.month_of_year 必须在 1 到 12 之间";
        return parsed;
    }
    parsed.month_of_year = month.has_value() ? std::optional<uint8_t>{static_cast<uint8_t>(*month)} : std::nullopt;
    const auto count = JsonInteger(*repeat, "occurrence_count");
    if (repeat->Get("occurrence_count") != nullptr && (!count.has_value() || *count < 1 || *count > INT32_MAX)) {
        parsed.error = "repeat.occurrence_count 必须是正整数";
        return parsed;
    }
    parsed.occurrence_count = count.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*count)} : std::nullopt;

    if (require_anchor &&
        (!parsed.freq_type.has_value() || !parsed.start_time.has_value() || !parsed.start_date.has_value())) {
        parsed.error = "repeat 必须包含 freq_type、start_date 和 start_time";
    }
    return parsed;
}

ParsedRepeat ParseRuleProperties(const PropertyList& properties, bool require_anchor) {
    return ParseFlat(properties, require_anchor);
}

schedule::CreateScheduleRuleCommand CreateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat) {
    schedule::CreateScheduleRuleCommand command;
    command.event = properties.value<std::string>("event").value_or("");
    command.location = properties.value<std::string>("location");
    command.notes = properties.value<std::string>("notes");
    command.freq_type = repeat.freq_type.value_or(schedule::Frequency::kDaily);
    command.interval_val = repeat.interval_val.value_or(1);
    command.weekdays_mask = repeat.weekdays_mask;
    command.day_of_month = repeat.day_of_month;
    command.month_of_year = repeat.month_of_year;
    command.monthly_mode = repeat.monthly_mode;
    command.start_time = repeat.start_time.value_or(schedule::LocalTime{});
    command.start_date = repeat.start_date;
    command.end_time = repeat.end_time;
    command.end_date = repeat.end_date;
    command.occurrence_count = repeat.occurrence_count;
    command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
    return command;
}

schedule::UpdateScheduleRuleCommand UpdateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat) {
    schedule::UpdateScheduleRuleCommand command;
    command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
    command.event = properties.value<std::string>("event");
    if (properties.value<std::string>("location").has_value()) {
        command.location = *properties.value<std::string>("location");
    }
    if (properties.value<std::string>("notes").has_value()) {
        command.notes = *properties.value<std::string>("notes");
    }
    if (repeat.freq_type.has_value()) command.freq_type = repeat.freq_type;
    if (repeat.interval_val.has_value()) command.interval_val = repeat.interval_val;
    if (repeat.weekdays_mask.has_value()) command.weekdays_mask = repeat.weekdays_mask;
    if (repeat.day_of_month.has_value()) command.day_of_month = repeat.day_of_month;
    if (repeat.month_of_year.has_value()) command.month_of_year = repeat.month_of_year;
    if (repeat.monthly_mode.has_value()) command.monthly_mode = repeat.monthly_mode;
    if (repeat.start_time.has_value()) command.start_time = repeat.start_time;
    if (repeat.end_time.has_value()) command.end_time = repeat.end_time;
    if (repeat.start_date.has_value()) command.start_date = repeat.start_date;
    if (repeat.end_date.has_value()) command.end_date = repeat.end_date;
    if (repeat.occurrence_count.has_value()) command.occurrence_count = repeat.occurrence_count;
    command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
    return command;
}

PropertyList CreateProperties() {
    return PropertyList({
        Property("event", PropertyType::kString).with_description("一次性日程标题或事件内容"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("一次性日程开始时间，格式 YYYY-MM-DD HH:mm:ss。不传表示无明确开始时间"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("一次性日程结束时间，格式 YYYY-MM-DD HH:mm:ss。不传表示无明确结束时间"),
        Property::Optional("location", PropertyType::kString).with_description("一次性日程地点"),
        Property::Optional("notes", PropertyType::kString).with_description("一次性日程备注"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("是否忽略时间冲突；为 true 时直接创建并返回创建后的日程"),
    });
}

PropertyList CreateRuleProperties() {
    return PropertyList({
        Property("event", PropertyType::kString).with_description("周期日程标题或事件内容"),
        Property("freq_type", PropertyType::kString)
            .with_description("周期频率，只能是 daily、weekly、monthly、yearly"),
        Property("start_date", PropertyType::kString).with_description("周期规则开始日期，格式 YYYY-MM-DD"),
        Property("start_time", PropertyType::kString).with_description("每次 occurrence 的开始时间，格式 HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("每次 occurrence 的结束时间，格式 HH:mm:ss"),
        Property::Optional("location", PropertyType::kString).with_description("周期日程地点"),
        Property::Optional("notes", PropertyType::kString).with_description("周期日程备注"),
        Property("interval_val", PropertyType::kInteger, int64_t{1})
            .with_description("重复间隔，默认 1，必须为正整数"),
        Property::Optional("weekdays_mask", PropertyType::kInteger)
            .with_description("仅 weekly 使用，按位表示星期一至星期日，范围 1 到 127"),
        Property::Optional("day_of_month", PropertyType::kInteger)
            .with_description("monthly 或 yearly 按固定日期重复时使用，范围 1 到 31"),
        Property::Optional("month_of_year", PropertyType::kInteger)
            .with_description("仅 yearly 使用，表示月份，范围 1 到 12"),
        Property::Optional("monthly_mode", PropertyType::kString)
            .with_description("仅 monthly 使用，只能是 specific_day 或 last_day"),
        Property::Optional("end_date", PropertyType::kString).with_description("周期结束日期，格式 YYYY-MM-DD"),
        Property::Optional("occurrence_count", PropertyType::kInteger).with_description("最多发生次数"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("是否忽略首条实例与已有日程的时间冲突"),
    });
}

PropertyList QueryProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger)
            .with_description("查询一条已物化 schedule；可指向一次性日程或周期实例。与 rule_id 互斥"),
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("查询一条周期规则及其已物化实例、未来 occurrence、exception；与 schedule_id 互斥"),
        Property::Optional("keyword", PropertyType::kString).with_description("按日程标题或备注模糊搜索"),
        Property("status", PropertyType::kString, std::string("active"))
            .with_description("日程状态筛选，取值为 all、active、cancelled、completed"),
        Property::Optional("start_date", PropertyType::kString).with_description("查询开始日期，格式 YYYY-MM-DD"),
        Property::Optional("end_date", PropertyType::kString).with_description("查询结束日期，格式 YYYY-MM-DD"),
    });
}

PropertyList UpdateProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger)
            .with_description("要修改的 schedule 表记录 ID；由 schedule.query 返回。不要传 rule_id 或 original_start_time"),
        Property::Optional("event", PropertyType::kString).with_description("新的日程标题"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("新的开始时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("新的结束时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("location", PropertyType::kString).with_description("新的地点"),
        Property::Optional("notes", PropertyType::kString).with_description("新的备注"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}).with_description("是否忽略时间冲突"),
    });
}

PropertyList UpdateOccurrenceProperties() {
    return PropertyList({
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("周期规则 ID；只用于定位未来 occurrence"),
        Property::Optional("original_start_time", PropertyType::kString)
            .with_description("未来 occurrence 的原始发生时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("event", PropertyType::kString).with_description("仅覆盖这一次 occurrence 的标题"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("仅覆盖这一次 occurrence 的开始时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("仅覆盖这一次 occurrence 的结束时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("location", PropertyType::kString).with_description("仅覆盖这一次 occurrence 的地点"),
        Property::Optional("notes", PropertyType::kString).with_description("仅覆盖这一次 occurrence 的备注"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}).with_description("是否忽略时间冲突"),
    });
}

PropertyList UpdateRuleProperties() {
    return PropertyList({
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("要修改的整条周期规则 ID；由 schedule.query 返回。不要传 schedule_id 或 original_start_time"),
        Property::Optional("event", PropertyType::kString).with_description("新的规则标题"),
        Property::Optional("freq_type", PropertyType::kString)
            .with_description("新的周期频率，只能是 daily、weekly、monthly、yearly"),
        Property::Optional("start_date", PropertyType::kString).with_description("新的规则开始日期，格式 YYYY-MM-DD"),
        Property::Optional("start_time", PropertyType::kString).with_description("新的规则开始时间，格式 HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString).with_description("新的规则结束时间，格式 HH:mm:ss"),
        Property::Optional("location", PropertyType::kString).with_description("新的规则地点"),
        Property::Optional("notes", PropertyType::kString).with_description("新的规则备注"),
        Property::Optional("interval_val", PropertyType::kInteger).with_description("新的重复间隔"),
        Property::Optional("weekdays_mask", PropertyType::kInteger)
            .with_description("weekly 新的星期掩码，范围 1 到 127"),
        Property::Optional("day_of_month", PropertyType::kInteger).with_description("新的固定日期，范围 1 到 31"),
        Property::Optional("month_of_year", PropertyType::kInteger).with_description("yearly 新的月份，范围 1 到 12"),
        Property::Optional("monthly_mode", PropertyType::kString)
            .with_description("monthly 新模式，只能是 specific_day 或 last_day"),
        Property::Optional("end_date", PropertyType::kString).with_description("新的结束日期，格式 YYYY-MM-DD"),
        Property::Optional("occurrence_count", PropertyType::kInteger).with_description("新的最大发生次数"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}).with_description("是否忽略规则重建后的冲突"),
    });
}

PropertyList DeleteProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger)
            .with_description("要取消的 schedule 表记录 ID，可指向一次性日程或已物化周期实例；由 schedule.query 返回。不要传 rule_id 或 original_start_time"),
        Property::Optional("expected_event", PropertyType::kString)
            .with_description("删除前必须从 schedule.query 原样回传该记录的 event，用于确认不会取消错误目标"),
        Property::Optional("expected_start_time", PropertyType::kString)
            .with_description("删除前必须从 schedule.query 原样回传该记录的 start_time；无开始时间的记录不能通过此确认工具取消"),
    });
}

PropertyList DeleteRuleProperties() {
    return PropertyList({
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("要取消的整条周期规则 ID；会停止后续 occurrence。不要传 schedule_id 或 original_start_time"),
    });
}

PropertyList SkipOccurrenceProperties() {
    return PropertyList({
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("周期规则 ID；只用于定位一个未来 occurrence"),
        Property::Optional("original_start_time", PropertyType::kString)
            .with_description("要跳过的原始 occurrence 完整本地开始时间，严格使用 YYYY-MM-DD HH:mm:ss；不是规则的 HH:mm:ss 时间部分"),
        Property::Optional("expected_event", PropertyType::kString)
            .with_description("从 schedule.query 的 future_occurrences 原样回传 event，用于确认跳过的是正确 occurrence"),
    });
}

PropertyList OperationQueryProperties() {
    return PropertyList({
        Property::Optional("entity_type", PropertyType::kString)
            .with_description("操作对象类型，取值为 schedule、rule、exception"),
        Property::Optional("type", PropertyType::kString).with_description("操作类型，取值为 create、update、delete"),
        Property::Optional("keyword", PropertyType::kString).with_description("按操作对象名称模糊搜索"),
    });
}

}  // namespace voicelife::mcp::schedule_tool_input
