#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::mcp::schedule_tool_input {

/** @brief 解析 repeat 对象后的周期规则字段。 */
struct ParsedRepeat {
    std::optional<schedule::Frequency> freq_type;
    std::optional<schedule::LocalTime> start_time;
    std::optional<schedule::LocalTime> end_time;
    std::optional<schedule::LocalDate> start_date;
    std::optional<schedule::LocalDate> end_date;
    std::optional<int32_t> interval_val;
    std::optional<uint8_t> weekdays_mask;
    std::optional<uint8_t> day_of_month;
    std::optional<uint8_t> month_of_year;
    std::optional<schedule::MonthlyMode> monthly_mode;
    std::optional<int32_t> occurrence_count;
    std::string error;

    /** @brief 判断解析是否成功。 @return 无错误时返回 true。 */
    [[nodiscard]] bool ok() const { return error.empty(); }
};

/**
 * @brief 解析 repeat 参数对象。
 * @param repeat 可选的 repeat 对象。
 * @param require_anchor 是否要求创建周期规则时必须包含 freq_type、start_date 和 start_time。
 * @return 解析后的周期字段或错误。
 */
ParsedRepeat ParseRepeat(const std::optional<JsonValue>& repeat, bool require_anchor);

/**
 * @brief 从 MCP 参数和 repeat 字段构造创建周期规则命令。
 * @param properties MCP 调用参数。
 * @param repeat 解析后的 repeat 字段。
 * @return 创建周期规则命令。
 */
schedule::CreateScheduleRuleCommand CreateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat);

/**
 * @brief 从 MCP 参数和 repeat 字段构造更新周期规则命令。
 * @param properties MCP 调用参数。
 * @param repeat 解析后的 repeat 字段。
 * @return 更新周期规则命令。
 */
schedule::UpdateScheduleRuleCommand UpdateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat);

/** @brief 创建 schedule.create 工具参数定义。 @return 参数定义。 */
PropertyList CreateProperties();

/** @brief 创建 schedule.query 工具参数定义。 @return 参数定义。 */
PropertyList QueryProperties();

/** @brief 创建 schedule.update 工具参数定义。 @return 参数定义。 */
PropertyList UpdateProperties();

/** @brief 创建 schedule.delete 工具参数定义。 @return 参数定义。 */
PropertyList DeleteProperties();

}  // namespace voicelife::mcp::schedule_tool_input
