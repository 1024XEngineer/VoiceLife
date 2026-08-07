#include "schedule_operation_query_helpers.h"

#include <algorithm>
#include <chrono>

namespace voicelife::schedule {

std::vector<OperationRecord> FilterRecentScheduleOperations(std::vector<OperationRecord> operations, DateTime now) {
    const DateTime earliest = now - std::chrono::minutes{15};
    operations.erase(std::remove_if(operations.begin(), operations.end(),
                                    [earliest, now](const auto& operation) {
                                        return operation.operated_at < earliest || operation.operated_at > now;
                                    }),
                     operations.end());

    std::sort(operations.begin(), operations.end(), [](const auto& left, const auto& right) {
        if (left.operated_at != right.operated_at) return left.operated_at > right.operated_at;
        return left.id > right.id;
    });
    return operations;
}

}  // namespace voicelife::schedule
