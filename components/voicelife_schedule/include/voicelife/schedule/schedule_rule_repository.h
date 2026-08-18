#pragma once

#include <optional>
#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 定义周期规则所需的持久化能力。
 *
 * 业务服务只依赖这个接口，不关心 SQLite 连接、SQL 文本或字段映射。
 * 跨表的原子操作（创建规则同时物化首条实例、修改规则并重建未来实例）由具体仓储实现，
 * 以保证规则、实例和例外在同一事务中提交。
 */
class ScheduleRuleRepository {
   public:
    /** @brief 允许通过接口类型释放仓储对象。 */
    virtual ~ScheduleRuleRepository() = default;

    /** @brief 插入一条周期规则。 @param rule 待插入规则；id 为零时由仓储生成标识和时间戳。 @return 保存后的完整规则。
     */
    virtual Result<ScheduleRule> Insert(const ScheduleRule& rule) = 0;

    /** @brief 更新已有规则的全部持久化字段。 @param rule 包含有效 id 的规则。 @return 更新结果。 */
    virtual Status Update(const ScheduleRule& rule) = 0;

    /** @brief 读取仓储中的全部规则。 @return 规则集合或数据库错误。 */
    [[nodiscard]] virtual Result<std::vector<ScheduleRule>> FindAll() const = 0;

    /** @brief 按标识读取一条规则。 @param id 规则标识。 @return 规则或未找到错误。 */
    [[nodiscard]] virtual Result<ScheduleRule> FindById(ScheduleRuleId id) const = 0;

    /**
     * @brief 在同一事务中创建规则并物化其首条实例。
     * @param rule 待创建规则。
     * @param first_instance 待物化的首条实例；无下一发生时间时为空。
     * @return 保存后的规则。
     */
    virtual Result<ScheduleRule> CreateWithFirstInstance(const ScheduleRule& rule,
                                                         const std::optional<Schedule>& first_instance) = 0;

    /**
     * @brief 在同一事务中更新规则、删除未发生的未来实例和例外，并物化新规则的首条实例。
     * @param rule 更新后的规则（含有效 id）。
     * @param first_instance 按新规则物化的首条实例；无下一发生时间时为空。
     * @return 更新后的规则。
     */
    virtual Result<ScheduleRule> UpdateAndRebuild(const ScheduleRule& rule,
                                                  const std::optional<Schedule>& first_instance) = 0;

    /**
     * @brief 在同一事务中取消规则、全部已创建实例，并清理该规则的例外。
     * @param id 规则标识。
     * @param cancelled_instance_count 输出被标记取消的实例数量。
     * @return 更新结果。
     */
    virtual Status CancelRuleAndInstances(ScheduleRuleId id, int64_t& cancelled_instance_count) = 0;

    /**
     * @brief 在单个事务中插入日程实例，并可选地将单次例外关联到该实例。
     * @param schedule 待插入日程实例。
     * @param linked_exception 需要回写 schedule_id 的单次例外；可为空。
     * @return 实际保存后的日程实例。
     */
    virtual Result<Schedule> CreateNextInstance(const Schedule& schedule,
                                                const std::optional<ScheduleException>& linked_exception) = 0;
};

}  // namespace voicelife::schedule
