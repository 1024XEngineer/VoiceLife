#include "voicelife/mcp/schedule_tools.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"
#include "voicelife/schedule/schedule_service.h"

namespace voicelife::mcp {
namespace {

using schedule::DateTime;

ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

std::string UnixTime(const DateTime& value) { return std::to_string(value.time_since_epoch().count()); }

void AddScheduleOutput(const schedule::Schedule& value, ToolResult& result) {
    result.output["id"] = std::to_string(value.id);
    result.output["event"] = value.event;
    result.output["status"] = std::to_string(static_cast<int>(value.status));
    if (value.start_time.has_value()) result.output["start_time"] = UnixTime(*value.start_time);
    if (value.end_time.has_value()) result.output["end_time"] = UnixTime(*value.end_time);
    if (value.location.has_value()) result.output["location"] = *value.location;
    if (value.notes.has_value()) result.output["notes"] = *value.notes;
}

std::optional<DateTime> ToUnixTime(const PropertyList& properties, const char* name) {
    const auto value = properties.value<int64_t>(name);
    return value.has_value() ? std::optional<DateTime>(std::chrono::seconds{*value}) : std::nullopt;
}

schedule::ScheduleStatusFilter ParseStatus(const std::string& value, Status& status) {
    if (value == "all") return schedule::ScheduleStatusFilter::kAll;
    if (value == "active") return schedule::ScheduleStatusFilter::kActive;
    if (value == "cancelled") return schedule::ScheduleStatusFilter::kCancelled;
    if (value == "completed") return schedule::ScheduleStatusFilter::kCompleted;
    status = Status::Error(ErrorCode::kInvalidArgument, "status 必须是 all、active、cancelled 或 completed");
    return schedule::ScheduleStatusFilter::kActive;
}

PropertyList CreateProperties() {
    return PropertyList({
        Property("event", PropertyType::kString),
        Property::Optional("start_time", PropertyType::kInteger),
        Property::Optional("end_time", PropertyType::kInteger),
        Property::Optional("location", PropertyType::kString),
        Property::Optional("notes", PropertyType::kString),
        Property("ignore_conflict", PropertyType::kBoolean, false),
    });
}

PropertyList QueryProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger),
        Property::Optional("keyword", PropertyType::kString),
        Property::Optional("start_from", PropertyType::kInteger),
        Property::Optional("start_to", PropertyType::kInteger),
        Property("status", PropertyType::kString, std::string("active")),
        Property("limit", PropertyType::kInteger, int64_t{10}),
        Property("offset", PropertyType::kInteger, int64_t{0}),
    });
}

}  // namespace

Status RegisterScheduleTools(McpServer& server, schedule::ScheduleService& service) {
    Status status =
        server.add_tool("schedule.create", "创建一条日程；时间参数使用 Unix 秒。", CreateProperties(),
                        [&service](const ToolCall& call, const PropertyList& properties) {
                            schedule::CreateScheduleCommand command;
                            command.event = properties.value<std::string>("event").value();
                            command.start_time = ToUnixTime(properties, "start_time");
                            command.end_time = ToUnixTime(properties, "end_time");
                            command.location = properties.value<std::string>("location");
                            command.notes = properties.value<std::string>("notes");
                            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
                            command.idempotency_key = call.request_id;
                            const auto result = service.create_schedule(command);
                            if (!result.status.ok()) return Failure(result.status);
                            ToolResult output{.status = result.status, .output = {{"message", result.message}}};
                            if (result.schedule.has_value()) AddScheduleOutput(*result.schedule, output);
                            output.output["conflict_count"] = std::to_string(result.conflicts.size());
                            output.output["nearby_count"] = std::to_string(result.nearby_schedules.size());
                            return output;
                        });
    if (!status.ok()) return status;

    return server.add_tool(
        "schedule.query", "查询日程；时间筛选使用 Unix 秒。", QueryProperties(),
        [&service](const ToolCall&, const PropertyList& properties) {
            Status parse_status = Status::Ok();
            schedule::QueryScheduleCommand command;
            command.schedule_id = properties.value<int64_t>("schedule_id");
            command.keyword = properties.value<std::string>("keyword");
            command.start_from = ToUnixTime(properties, "start_from");
            command.start_to = ToUnixTime(properties, "start_to");
            command.status = ParseStatus(properties.value<std::string>("status").value_or("active"), parse_status);
            command.limit = properties.value<int64_t>("limit").value_or(10);
            command.offset = properties.value<int64_t>("offset").value_or(0);
            if (!parse_status.ok()) return Failure(parse_status);
            const auto result = service.query_schedule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status, .output = {{"total", std::to_string(result.total)}}};
            output.output["count"] = std::to_string(result.schedules.size());
            for (std::size_t index = 0; index < result.schedules.size(); ++index) {
                ToolResult item{.status = Status::Ok(), .output = {}};
                AddScheduleOutput(result.schedules[index], item);
                for (const auto& [key, value] : item.output) {
                    output.output["schedule_" + std::to_string(index) + "_" + key] = value;
                }
            }
            return output;
        });
}

}  // namespace voicelife::mcp
