#pragma once

#include <vector>

#include "voicelife/contracts/status.h"
#include "voicelife/schedule/schedule_types.h"

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

    /**
     * @brief 读取仓储中的全部日程。
     * @return 日程集合或数据库错误。
     */
    [[nodiscard]] virtual Result<std::vector<Schedule>> FindAll() const = 0;
};

}  // namespace voicelife::schedule
