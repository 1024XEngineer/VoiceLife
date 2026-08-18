#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace voicelife::schedule {

/// 日程、操作记录和提醒使用数据库兼容的 64 位整数标识。
using ScheduleId = int64_t;
using OperationId = int64_t;
/// 模块内部的日期时间精确到秒；模型字符串与该类型的转换由边界适配器负责。
using DateTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;

/// 日程持久化状态。
enum class ScheduleStatus {
    kActive = 1,
    kCancelled = 2,
    kCompleted = 3,
};

/// 日程查询使用的状态筛选条件。
enum class ScheduleStatusFilter { kAll, kActive, kCancelled, kCompleted };

/// 操作对象实体类型：决定 before 快照的结构，以及查询时的实体维度。
enum class OperationEntityType { kSchedule = 1, kRule = 2, kException = 3 };

/// 可记录的操作类型；撤销没有独立接口，回滚动作按其自然类型被记录。
enum class ScheduleOperationType { kCreate = 1, kUpdate = 2, kDelete = 3 };

/// 日程实体，对应 Schedule 数据表。
struct Schedule {
    ScheduleId id = 0;
    std::string event;
    std::optional<DateTime> start_time;
    std::optional<DateTime> end_time;
    std::optional<std::string> location;
    std::optional<std::string> notes;
    /// 周期规则来源标识；当前数据库不建立外键。
    std::optional<ScheduleId> rule_id;
    ScheduleStatus status = ScheduleStatus::kActive;
    DateTime created_at;
    DateTime updated_at;
};

/// 日程操作记录，对应 OperationRecord 数据表。
struct OperationRecord {
    OperationId id = 0;
    OperationEntityType entity_type = OperationEntityType::kSchedule;
    ScheduleOperationType type = ScheduleOperationType::kCreate;
    int64_t entity_id = 0;
    DateTime operated_at;              ///< 仓储盖章，不来自调用方
    std::string label;                 ///< 展示用名称（日程名 / 规则名 / 例外描述）
    std::optional<std::string> before; ///< 操作前快照 JSON；kCreate 必须为空
};

/// 周期规则与单次例外使用的数据库兼容 64 位整数标识。
using ScheduleRuleId = int64_t;
using ScheduleExceptionId = int64_t;

/// 周期频率。
enum class Frequency { kDaily = 1, kWeekly = 2, kMonthly = 3, kYearly = 4 };

/// 月规则模式：指定日期或当月最后一天。
enum class MonthlyMode { kSpecificDay = 1, kLastDay = 2 };

/// 单次例外类型：修改或跳过。
enum class ExceptionType { kModify = 1, kSkip = 2 };

/// 本地日期（东八区 civil date）。
struct LocalDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

/// 本地时刻（东八区 civil time，精确到秒）。
struct LocalTime {
    int hour = 0;
    int minute = 0;
    int second = 0;
};

/// 周期规则实体，对应 ScheduleRule 数据表。
struct ScheduleRule {
    ScheduleRuleId id = 0;
    std::string event;
    std::optional<std::string> location;
    std::optional<std::string> notes;
    Frequency freq_type = Frequency::kDaily;
    int32_t interval_val = 1;
    std::optional<uint8_t> weekdays_mask;
    std::optional<uint8_t> day_of_month;
    std::optional<uint8_t> month_of_year;
    std::optional<MonthlyMode> monthly_mode;
    LocalTime start_time;
    std::optional<LocalTime> end_time;
    LocalDate start_date;
    std::optional<LocalDate> end_date;
    std::optional<int32_t> occurrence_count;
    ScheduleStatus status = ScheduleStatus::kActive;
    DateTime created_at;
    DateTime updated_at;
};

/// 单次例外实体，对应 ScheduleException 数据表。
struct ScheduleException {
    ScheduleExceptionId id = 0;
    ScheduleRuleId rule_id = 0;
    DateTime original_start_time;
    std::optional<ScheduleId> schedule_id;
    ExceptionType type = ExceptionType::kModify;
    std::optional<DateTime> override_start_time;
    std::optional<DateTime> override_end_time;
    std::optional<std::string> override_event;
    std::optional<std::string> override_location;
    std::optional<std::string> override_notes;
    DateTime created_at;
    DateTime updated_at;
};

}  // namespace voicelife::schedule
