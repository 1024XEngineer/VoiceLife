#include "schedule_mcp_tools_input.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

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

PropertyList RepeatProperties() {
    return PropertyList({
        Property("freq_type", PropertyType::kString)
            .with_description(
                "周期频率，只能取 daily、weekly、monthly、yearly。不要把不支持的自然语言规则近似成另一种频率"),
        Property("interval_val", PropertyType::kInteger, int64_t{1})
            .with_description("周期间隔，例如 1 表示每天、每周、每月或每年一次"),
        Property("start_date", PropertyType::kString).with_description("周期规则开始日期，格式 YYYY-MM-DD"),
        Property("start_time", PropertyType::kString).with_description("周期日程每日开始时间，格式 HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString).with_description("周期日程每日结束时间，格式 HH:mm:ss"),
        Property::Optional("end_date", PropertyType::kString).with_description("周期规则结束日期，格式 YYYY-MM-DD"),
        Property::Optional("occurrence_count", PropertyType::kInteger).with_description("周期规则最多发生的次数"),
        Property::Optional("weekdays_mask", PropertyType::kInteger)
            .with_description("每周重复的星期掩码，weekly 模式使用；只表达指定星期，不表达每月第 N 个星期几"),
        Property::Optional("day_of_month", PropertyType::kInteger)
            .with_description("每月或每年重复的固定日期；不支持每月第 N 个星期几或最后一个工作日"),
        Property::Optional("month_of_year", PropertyType::kInteger).with_description("每年重复的月份，yearly 模式使用"),
        Property::Optional("monthly_mode", PropertyType::kString)
            .with_description("月重复模式，只能取 specific_day 或 last_day；不支持 ordinal weekday、工作日等相对规则"),
    });
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
        Property("event", PropertyType::kString).with_description("日程标题或事件内容"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("一次性日程开始时间，格式 YYYY-MM-DD HH:mm:ss。不传表示无明确开始时间"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("一次性日程结束时间，格式 YYYY-MM-DD HH:mm:ss。不传表示无明确结束时间"),
        Property::Optional("location", PropertyType::kString).with_description("日程地点"),
        Property::Optional("notes", PropertyType::kString).with_description("日程备注"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("是否忽略时间冲突；为 true 时直接创建并返回创建后的日程"),
        Property::OptionalObject("repeat", RepeatProperties())
            .with_description("周期规则。不传时创建一次性日程，传入时创建周期日程并生成未来实例"),
    });
}

PropertyList QueryProperties() {
    return PropertyList({
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
            .with_description("更新或取消已物化日程时使用的日程 ID，由 schedule.query 返回"),
        Property::Optional("expected_event", PropertyType::kString)
            .with_description("取消单次日程时，必须逐字回传 schedule.query 返回的 event 以确认目标"),
        Property::Optional("expected_start_time", PropertyType::kString)
            .with_description("取消单次日程时，必须逐字回传 schedule.query 返回的 start_time 以确认目标"),
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("更新未来周期实例或整条周期规则时使用的规则 ID"),
        Property::Optional("original_start_time", PropertyType::kString)
            .with_description("未来周期实例的原始发生时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("event", PropertyType::kString).with_description("新的日程标题"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("新的开始时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("新的结束时间，格式 YYYY-MM-DD HH:mm:ss"),
        Property::Optional("location", PropertyType::kString).with_description("新的地点"),
        Property::Optional("notes", PropertyType::kString).with_description("新的备注"),
        Property::Optional("status", PropertyType::kString)
            .with_description("更新日程状态；跳过某次周期日程时传 cancelled，恢复时传 active"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}).with_description("是否忽略时间冲突"),
        Property::OptionalObject("repeat", RepeatProperties()).with_description("更新周期规则时使用的新周期配置"),
    });
}

PropertyList DeleteProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger)
            .with_description("要删除或取消的单次日程 ID；必须先由 schedule.query 返回"),
        Property::Optional("expected_event", PropertyType::kString)
            .with_description("按 schedule_id 删除时，必须逐字回传 schedule.query 返回的 event 以确认目标"),
        Property::Optional("expected_start_time", PropertyType::kString)
            .with_description("按 schedule_id 删除时，必须逐字回传 schedule.query 返回的 start_time 以确认目标"),
        Property::Optional("rule_id", PropertyType::kInteger).with_description("要删除或取消的周期规则 ID"),
        Property::Optional("original_start_time", PropertyType::kString)
            .with_description("删除未来周期单次时使用的原始发生时间，格式 YYYY-MM-DD HH:mm:ss"),
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
