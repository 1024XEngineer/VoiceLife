#pragma once

#include <optional>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_commands.h"

namespace voicelife::schedule {

/**
 * @brief 定义日程业务所需的持久化能力。
 *
 * 业务服务只依赖这个接口，不关心 SQLite 连接、SQL 文本或字段映射。
 */
class ScheduleRepository {
   public:
    /** @brief 允许通过接口类型释放仓储对象。 */
    virtual ~ScheduleRepository() = default;

    /**
     * @brief 插入一条日程。
     * @param schedule 待插入的日程；id 为零时由仓储生成标识和时间戳。
     * @return 实际保存后的完整日程，失败时返回错误状态。
     */
    virtual Result<Schedule> Insert(const Schedule& schedule) = 0;

    /** @brief 更新已有日程的全部持久化字段。 @param schedule 包含有效 id 的日程。 @return 更新结果。 */
    virtual Status Update(const Schedule& schedule) {
        (void)schedule;
        return Status::Error(ErrorCode::kUnavailable, "当前仓储不支持更新日程");
    }

    /**
     * @brief 将指定日程原子地标记为已取消，保留历史数据。
     * @param id 日程标识。
     * @return 首次取消成功返回成功；不存在返回 kNotFound；已取消返回 kConflict。
     */
    virtual Status Delete(ScheduleId id) {
        (void)id;
        return Status::Error(ErrorCode::kUnavailable, "当前仓储不支持删除日程");
    }

    /**
     * @brief 按标识读取一条日程。
     * @param id 日程标识。
     * @return 日程；不存在时返回 kNotFound。
     */
    [[nodiscard]] virtual Result<Schedule> FindById(ScheduleId id) const {
        (void)id;
        return Result<Schedule>::Failure(ErrorCode::kUnavailable, "当前仓储不支持按 ID 查询日程");
    }

    /**
     * @brief 按筛选条件读取当前页日程。
     * @param query 日程查询条件。
     * @return 当前页日程集合。
     */
    [[nodiscard]] virtual Result<std::vector<Schedule>> Find(const QueryScheduleCommand& query) const {
        (void)query;
        return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, "当前仓储不支持条件查询日程");
    }

    /** @brief 按筛选条件统计总数，不受 limit/offset 影响。 */
    [[nodiscard]] virtual Result<int64_t> Count(const QueryScheduleCommand& query) const {
        (void)query;
        return Result<int64_t>::Failure(ErrorCode::kUnavailable, "当前仓储不支持统计日程");
    }

    /**
     * @brief 查询与给定时间窗口可能重叠或临近的有效日程。
     * @param start 窗口起点。
     * @param end 窗口终点；单点日程应传同一时间。
     * @param exclude_id 排除的日程标识。
     * @return 有开始时间且可能重叠的有效日程集合。
     */
    [[nodiscard]] virtual Result<std::vector<Schedule>> FindOverlapping(
        DateTime start, DateTime end, std::optional<ScheduleId> exclude_id) const {
        (void)start;
        (void)end;
        (void)exclude_id;
        return Result<std::vector<Schedule>>::Failure(ErrorCode::kUnavailable, "当前仓储不支持时间窗口查询日程");
    }

    /**
     * @brief 读取仓储中的全部日程。
     * @return 日程集合或数据库错误。
     */
    [[nodiscard]] virtual Result<std::vector<Schedule>> FindAll() const = 0;
};

}  // namespace voicelife::schedule
