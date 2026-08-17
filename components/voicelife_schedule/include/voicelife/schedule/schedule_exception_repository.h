#pragma once

#include <optional>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 定义单次例外所需的持久化能力。
 *
 * 例外以 (rule_id, original_start_time) 为逻辑键，数据库以唯一约束保证同一 occurrence
 * 至多存在一条例外。业务服务只依赖本接口，不关心 SQLite 连接、SQL 文本或字段映射。
 */
class ScheduleExceptionRepository {
   public:
    /** @brief 允许通过接口类型释放仓储对象。 */
    virtual ~ScheduleExceptionRepository() = default;

    /**
     * @brief 插入或更新一条单次例外（按 rule_id + original_start_time 定位）。
     * @param exception 待写入例外；id 为零时由仓储生成标识和时间戳。
     * @return 保存后的完整例外。
     */
    virtual Result<ScheduleException> Upsert(const ScheduleException& exception) = 0;

    /** @brief 读取某规则的全部例外。 @param rule_id 规则标识。 @return 例外列表或数据库错误。 */
    [[nodiscard]] virtual Result<std::vector<ScheduleException>> FindByRule(ScheduleRuleId rule_id) const = 0;

    /**
     * @brief 按逻辑键读取一条例外。
     * @param rule_id 规则标识。
     * @param original_start_time 原始发生时间。
     * @return 例外；不存在时 value 为空。
     */
    [[nodiscard]] virtual Result<std::optional<ScheduleException>> FindByRuleAndTime(
        ScheduleRuleId rule_id, DateTime original_start_time) const = 0;

    /**
     * @brief 删除某规则在指定时间之后的未发生例外（用于整条规则重建）。
     * @param rule_id 规则标识。
     * @param after 删除该时间之后的例外（含边界由实现固定为不删除该时刻本身）。
     * @return 删除结果。
     */
    virtual Status DeleteFuture(ScheduleRuleId rule_id, DateTime after) = 0;
};

}  // namespace voicelife::schedule
