#pragma once

#include <vector>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::test::schedule_repository_test_data {

/** @brief 转换 Unix 秒。 @param seconds Unix 秒。 @return 日程时间。 */
inline schedule::DateTime At(int64_t seconds) { return schedule::DateTime{std::chrono::seconds{seconds}}; }

/**
 * @brief 返回创建、修改和删除服务测试使用的固定日程。
 * @return 与原日程模拟数据等价的独立集合。
 */
inline std::vector<schedule::Schedule> DefaultSchedules() {
    return {
        schedule::Schedule{
            .id = 1001,
            .event = "模拟团队周会",
            .start_time = At(1'800'000'000),
            .end_time = At(1'800'003'600),
            .location = std::nullopt,
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kActive,
            .created_at = At(1'799'900'000),
            .updated_at = At(1'799'900'000),
        },
        schedule::Schedule{
            .id = 1002,
            .event = "模拟单点日程",
            .start_time = At(1'800'007'200),
            .end_time = std::nullopt,
            .location = std::nullopt,
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kActive,
            .created_at = At(1'799'900'000),
            .updated_at = At(1'799'900'000),
        },
        schedule::Schedule{
            .id = 1003,
            .event = "模拟周期规则实例",
            .start_time = At(1'800'010'800),
            .end_time = At(1'800'014'400),
            .location = std::nullopt,
            .notes = std::nullopt,
            .rule_id = 3001,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kActive,
            .created_at = At(1'799'900'000),
            .updated_at = At(1'799'900'000),
        },
    };
}

/**
 * @brief 返回查询服务测试使用的固定日程。
 * @return 与原查询模拟数据等价的独立集合。
 */
inline std::vector<schedule::Schedule> QuerySchedules() {
    return {
        schedule::Schedule{
            .id = 2001,
            .event = "数据库连接评审",
            .start_time = At(1'810'000'000),
            .end_time = At(1'810'003'600),
            .location = "会议室 A",
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kActive,
            .created_at = At(1'809'900'000),
            .updated_at = At(1'809'900'000),
        },
        schedule::Schedule{
            .id = 2002,
            .event = "数据库连接复盘",
            .start_time = At(1'810'007'200),
            .end_time = std::nullopt,
            .location = "线上",
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kCompleted,
            .created_at = At(1'809'900'100),
            .updated_at = At(1'810'008'000),
        },
        schedule::Schedule{
            .id = 2003,
            .event = "产品方案讨论",
            .start_time = At(1'810'003'600),
            .end_time = At(1'810'005'400),
            .location = "会议室 B",
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kCancelled,
            .created_at = At(1'809'900'200),
            .updated_at = At(1'809'901'000),
        },
        schedule::Schedule{
            .id = 2004,
            .event = "整理周报",
            .start_time = std::nullopt,
            .end_time = std::nullopt,
            .location = std::nullopt,
            .notes = std::nullopt,
            .rule_id = std::nullopt,
            .reminder_task_id = std::nullopt,
            .status = schedule::ScheduleStatus::kActive,
            .created_at = At(1'809'900'300),
            .updated_at = At(1'809'900'300),
        },
    };
}

}  // namespace voicelife::test::schedule_repository_test_data
