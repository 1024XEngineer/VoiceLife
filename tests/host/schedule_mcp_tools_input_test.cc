#include "schedule_mcp_tools_input.h"

#include <cstdint>
#include <optional>
#include <string>

#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_rule_commands.h"

using voicelife::JsonValue;
using voicelife::mcp::PropertyList;
using voicelife::mcp::schedule_tool_input::CreateProperties;
using voicelife::mcp::schedule_tool_input::CreateRuleCommand;
using voicelife::mcp::schedule_tool_input::DeleteProperties;
using voicelife::mcp::schedule_tool_input::ParseRepeat;
using voicelife::mcp::schedule_tool_input::QueryProperties;
using voicelife::mcp::schedule_tool_input::UpdateProperties;
using voicelife::mcp::schedule_tool_input::UpdateRuleCommand;
using voicelife::schedule::Frequency;
using voicelife::schedule::MonthlyMode;
using voicelife::test::Check;

namespace {

/** @brief 构造 repeat 对象。 @return 完整的周期 repeat JSON 对象。 */
JsonValue RepeatObject() {
    JsonValue::ObjectMap fields;
    fields["freq_type"] = JsonValue::String("weekly");
    fields["start_date"] = JsonValue::String("2099-01-01");
    fields["start_time"] = JsonValue::String("09:00:00");
    fields["end_time"] = JsonValue::String("10:00:00");
    fields["interval_val"] = JsonValue::Number(2);
    fields["weekdays_mask"] = JsonValue::Number(3);
    fields["day_of_month"] = JsonValue::Number(5);
    fields["month_of_year"] = JsonValue::Number(6);
    fields["monthly_mode"] = JsonValue::String("specific_day");
    fields["occurrence_count"] = JsonValue::Number(7);
    return JsonValue::Object(std::move(fields));
}

}  // namespace

int main() {
    const auto parsed = ParseRepeat(std::optional<JsonValue>{RepeatObject()}, true);
    Check(parsed.ok() && parsed.freq_type == Frequency::kWeekly && parsed.interval_val == 2 &&
              parsed.weekdays_mask == 3 && parsed.day_of_month == 5 && parsed.month_of_year == 6 &&
              parsed.monthly_mode == MonthlyMode::kSpecificDay && parsed.occurrence_count == 7,
          "ParseRepeat 应解析完整 repeat 对象");

    const auto daily =
        ParseRepeat(std::optional<JsonValue>{JsonValue::Object({{"freq_type", JsonValue::String("daily")}})}, false);
    Check(daily.ok() && daily.freq_type == Frequency::kDaily, "daily 频率应解析成功");
    const auto monthly =
        ParseRepeat(std::optional<JsonValue>{JsonValue::Object({{"freq_type", JsonValue::String("monthly")}})}, false);
    Check(monthly.ok() && monthly.freq_type == Frequency::kMonthly, "monthly 频率应解析成功");
    const auto yearly =
        ParseRepeat(std::optional<JsonValue>{JsonValue::Object({{"freq_type", JsonValue::String("yearly")}})}, false);
    Check(yearly.ok() && yearly.freq_type == Frequency::kYearly, "yearly 频率应解析成功");
    const auto last_day = ParseRepeat(
        std::optional<JsonValue>{JsonValue::Object({{"monthly_mode", JsonValue::String("last_day")}})}, false);
    Check(last_day.ok() && last_day.monthly_mode == MonthlyMode::kLastDay, "last_day 月模式应解析成功");
    const auto empty_repeat = ParseRepeat(std::nullopt, false);
    Check(empty_repeat.ok(), "无 repeat 参数应保持成功状态");

    const auto missing_anchor = ParseRepeat(std::optional<JsonValue>{JsonValue::Object({})}, true);
    Check(!missing_anchor.ok(), "创建周期规则缺少 anchor 字段应失败");

    const auto bad_freq =
        ParseRepeat(std::optional<JsonValue>{JsonValue::Object({{"freq_type", JsonValue::String("bad")}})}, false);
    Check(!bad_freq.ok(), "无效 freq_type 应失败");
    const auto bad_time = ParseRepeat(
        std::optional<JsonValue>{JsonValue::Object({{"start_time", JsonValue::String("25:00:00")}})}, false);
    Check(!bad_time.ok(), "无效 start_time 应失败");
    const auto bad_date = ParseRepeat(
        std::optional<JsonValue>{JsonValue::Object({{"start_date", JsonValue::String("2099-13-01")}})}, false);
    Check(!bad_date.ok(), "无效 start_date 应失败");
    const auto bad_month =
        ParseRepeat(std::optional<JsonValue>{JsonValue::Object({{"monthly_mode", JsonValue::String("bad")}})}, false);
    Check(!bad_month.ok(), "无效 monthly_mode 应失败");

    const auto non_object = ParseRepeat(std::optional<JsonValue>{JsonValue::String("bad")}, false);
    Check(!non_object.ok(), "非对象 repeat 应失败");
    const auto bad_end_time =
        ParseRepeat(std::optional<JsonValue>{JsonValue::Object({{"end_time", JsonValue::String("99:00:00")}})}, false);
    Check(!bad_end_time.ok(), "无效 end_time 应失败");
    const auto bad_end_date = ParseRepeat(
        std::optional<JsonValue>{JsonValue::Object({{"end_date", JsonValue::String("2099-00-01")}})}, false);
    Check(!bad_end_date.ok(), "无效 end_date 应失败");

    PropertyList create_properties;
    const auto create = CreateRuleCommand(create_properties, parsed);
    Check(create.freq_type == Frequency::kWeekly && create.interval_val == 2 && create.weekdays_mask == 3 &&
              create.day_of_month == 5 && create.month_of_year == 6 &&
              create.monthly_mode == MonthlyMode::kSpecificDay && create.occurrence_count == 7,
          "CreateRuleCommand 应把 repeat 字段写入创建命令");

    PropertyList update_properties;
    const auto update = UpdateRuleCommand(update_properties, parsed);
    Check(update.freq_type == Frequency::kWeekly && update.interval_val == 2 && update.weekdays_mask == 3 &&
              update.day_of_month == 5 && update.month_of_year == 6 &&
              update.monthly_mode == MonthlyMode::kSpecificDay && update.occurrence_count == 7,
          "UpdateRuleCommand 应把 repeat 字段写入更新命令");

    Check(CreateProperties().to_schema().properties.contains("repeat"), "create 工具应声明 repeat 参数");
    Check(QueryProperties().to_schema().properties.contains("keyword"), "query 工具应声明 keyword 参数");
    Check(UpdateProperties().to_schema().properties.contains("repeat"), "update 工具应声明 repeat 参数");
    Check(DeleteProperties().to_schema().properties.contains("rule_id"), "delete 工具应声明 rule_id 参数");
    Check(DeleteProperties().to_schema().properties.contains("expected_event"), "delete 工具应声明目标确认事件");
    Check(DeleteProperties().to_schema().properties.contains("expected_start_time"), "delete 工具应声明目标确认时间");
    return 0;
}
