#include "schedule_operation_mock_data.h"

#include <chrono>
#include <cstddef>
#include <deque>

namespace voicelife::schedule {
namespace {

constexpr std::size_t kMaximumOperationRecordCount = 10;

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/** @brief 返回进程内有界的操作记录集合。 @return 可变的操作记录集合。 */
std::deque<OperationRecord>& MockOperations() {
    static std::deque<OperationRecord> operations;
    return operations;
}

/** @brief 返回下一条模拟操作记录 ID。 @return 尚未使用的正整数 ID。 */
OperationId& NextOperationId() {
    static OperationId next_id = 1;
    return next_id;
}

}  // namespace

Result<OperationRecord> AppendMockScheduleOperation(OperationRecord operation) {
    // TODO：真实存储接入后，替换为 OperationRecord Store 的原子 INSERT，并由存储层裁剪历史记录。
    operation.id = NextOperationId()++;
    operation.operated_at = Now();

    auto& operations = MockOperations();
    operations.push_back(std::move(operation));
    if (operations.size() > kMaximumOperationRecordCount) operations.pop_front();
    return Result<OperationRecord>::Success(operations.back());
}

std::vector<OperationRecord> LoadMockScheduleOperations() {
    const auto& operations = MockOperations();
    return {operations.begin(), operations.end()};
}

}  // namespace voicelife::schedule
