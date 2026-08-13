#include "schedule_mcp_tools.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_results.h"
#include "voicelife/schedule/schedule_service.h"

namespace voicelife::runtime {
namespace {

using mcp::Property;
using mcp::PropertyList;
using mcp::PropertyType;

ToolResult Failure(Status status) { return {.status = std::move(status), .output = {}}; }

std::string UnixTime(const schedule::DateTime& value) { return std::to_string(value.time_since_epoch().count()); }

void AddScheduleOutput(const schedule::Schedule& value, ToolResult& result) {
    result.output["id"] = std::to_string(value.id);
    result.output["event"] = value.event;
    result.output["status"] = std::to_string(static_cast<int>(value.status));
    if (value.start_time.has_value()) result.output["start_time"] = UnixTime(*value.start_time);
    if (value.end_time.has_value()) result.output["end_time"] = UnixTime(*value.end_time);
    if (value.location.has_value()) result.output["location"] = *value.location;
    if (value.notes.has_value()) result.output["notes"] = *value.notes;
}

std::optional<schedule::DateTime> ToUnixTime(const PropertyList& properties, const char* name) {
    const auto value = properties.value<int64_t>(name);
    return value.has_value() ? std::optional<schedule::DateTime>(std::chrono::seconds{*value}) : std::nullopt;
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

std::optional<schedule::ScheduleStatus> ParseScheduleStatus(const std::string& value) {
    if (value == "active") return schedule::ScheduleStatus::kActive;
    if (value == "cancelled") return schedule::ScheduleStatus::kCancelled;
    if (value == "completed") return schedule::ScheduleStatus::kCompleted;
    return std::nullopt;
}

PropertyList UpdateProperties() {
    return PropertyList({
        Property("schedule_id", PropertyType::kInteger),
        Property::Optional("event", PropertyType::kString),
        Property::Optional("start_time", PropertyType::kInteger),
        Property::Optional("end_time", PropertyType::kInteger),
        Property::Optional("location", PropertyType::kString),
        Property::Optional("notes", PropertyType::kString),
        Property::Optional("status", PropertyType::kString),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}),
    });
}

PropertyList DeleteProperties() {
    return PropertyList({
        Property("schedule_id", PropertyType::kInteger),
    });
}

}  // namespace

Status RegisterScheduleMcpTools(mcp::McpServer& server, schedule::ScheduleService& service) {
    Status status =
        server.add_tool("schedule.create", "创建一条日程；时间参数使用 Unix 秒。", CreateProperties(),
                        [&service](const PropertyList& properties) {
                            schedule::CreateScheduleCommand command;
                            command.event = properties.value<std::string>("event").value();
                            command.start_time = ToUnixTime(properties, "start_time");
                            command.end_time = ToUnixTime(properties, "end_time");
                            command.location = properties.value<std::string>("location");
                            command.notes = properties.value<std::string>("notes");
                            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
                            const auto result = service.create_schedule(command);
                            if (!result.status.ok()) return Failure(result.status);
                            ToolResult output{.status = result.status, .output = {{"message", result.message}}};
                            if (result.schedule.has_value()) AddScheduleOutput(*result.schedule, output);
                            output.output["conflict_count"] = std::to_string(result.conflicts.size());
                            output.output["nearby_count"] = std::to_string(result.nearby_schedules.size());
                            return output;
                        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.update", "修改一条一次性日程；时间参数使用 Unix 秒。", UpdateProperties(),
        [&service](const PropertyList& properties) {
            schedule::UpdateScheduleCommand command;
            command.schedule_id = properties.value<int64_t>("schedule_id").value_or(0);
            if (properties.value<std::string>("event").has_value()) {
                command.event = *properties.value<std::string>("event");
            }
            if (properties.value<int64_t>("start_time").has_value()) {
                command.start_time =
                    schedule::DateTime{std::chrono::seconds{*properties.value<int64_t>("start_time")}};
            }
            if (properties.value<int64_t>("end_time").has_value()) {
                command.end_time = schedule::DateTime{std::chrono::seconds{*properties.value<int64_t>("end_time")}};
            }
            if (properties.value<std::string>("location").has_value()) {
                command.location = *properties.value<std::string>("location");
            }
            if (properties.value<std::string>("notes").has_value()) {
                command.notes = *properties.value<std::string>("notes");
            }
            if (properties.value<std::string>("status").has_value()) {
                command.status = ParseScheduleStatus(*properties.value<std::string>("status"));
            }
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);

            const auto result = service.update_schedule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status, .output = {{"message", result.message}}};
            if (result.schedule.has_value()) AddScheduleOutput(*result.schedule, output);
            output.output["conflict_count"] = std::to_string(result.conflicts.size());
            return output;
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.delete", "取消一条一次性日程。", DeleteProperties(),
        [&service](const PropertyList& properties) {
            schedule::DeleteScheduleCommand command;
            command.schedule_id = properties.value<int64_t>("schedule_id").value_or(0);
            const auto result = service.delete_schedule(command);
            if (!result.status.ok()) return Failure(result.status);
            ToolResult output{.status = result.status,
                              .output = {{"schedule_id", std::to_string(result.schedule_id)},
                                         {"deleted", result.deleted ? "true" : "false"}}};
            return output;
        });
    if (!status.ok()) return status;

    return server.add_tool(
        "schedule.query", "查询日程；时间筛选使用 Unix 秒。", QueryProperties(),
        [&service](const PropertyList& properties) {
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
                for (const auto& [key, value] : item.output)
                    output.output["schedule_" + std::to_string(index) + "_" + key] = value;
            }
            return output;
        });
}

}  // namespace voicelife::runtime
