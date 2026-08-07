#include "schedule_operation_mock_data.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace voicelife::schedule {
namespace {

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/// 模拟存储中的操作条目；失效记录继续保留用于模拟审计数据。
struct StoredOperation {
    OperationRecord operation;
    bool active = true;
};

/// 进程内共享的模拟操作存储及其同步状态。
struct MockOperationStore {
    std::mutex mutex;
    std::deque<StoredOperation> operations;
    OperationId next_id = 1;
    std::optional<Status> next_undo_commit_failure;
};

/** @brief 返回进程内共享的模拟操作存储。 @return 可变的模拟操作存储。 */
MockOperationStore& Store() {
    static MockOperationStore store;
    return store;
}

/**
 * @brief 判断操作在指定时间点是否仍位于十五分钟撤销窗口内。
 * @param operation 待检查的操作记录。
 * @param now 用于计算撤销窗口的当前时间。
 * @return 操作时间不晚于当前时间且未早于十五分钟前时返回 true。
 */
bool IsWithinUndoWindow(const OperationRecord& operation, DateTime now) {
    const DateTime earliest = now - std::chrono::minutes{15};
    return operation.operated_at >= earliest && operation.operated_at <= now;
}

/**
 * @brief 在调用方持有存储锁时追加操作记录。
 * @param store 目标模拟存储。
 * @param operation 待追加的操作；操作 ID 和时间会被覆盖。
 * @param operated_at 要写入记录的操作时间。
 * @return 保存后的完整操作记录。
 */
OperationRecord AppendOperationLocked(MockOperationStore& store, OperationRecord operation, DateTime operated_at) {
    operation.id = store.next_id;
    operation.operated_at = operated_at;
    store.operations.push_back(StoredOperation{.operation = std::move(operation), .active = true});
    ++store.next_id;
    return store.operations.back().operation;
}

/**
 * @brief 在调用方持有存储锁时查找指定操作条目。
 * @param store 要查询的模拟存储。
 * @param operation_id 要查找的操作记录 ID。
 * @return 找到时返回条目地址，否则返回 nullptr。
 */
StoredOperation* FindOperationLocked(MockOperationStore& store, OperationId operation_id) {
    for (StoredOperation& stored : store.operations) {
        if (stored.operation.id == operation_id) return &stored;
    }
    return nullptr;
}

/**
 * @brief 校验指定操作条目当前是否仍可撤销。
 * @param stored 已按 ID 找到的操作条目；不存在时为空。
 * @param now 用于判断十五分钟撤销窗口的当前时间。
 * @return 可撤销时返回操作记录，失效、过期或不存在时返回失败。
 */
Result<OperationRecord> ValidateUndoableOperation(const StoredOperation* stored, DateTime now) {
    if (stored == nullptr || !stored->active) {
        return Result<OperationRecord>::Failure(ErrorCode::kNotFound, "操作不存在或已撤销");
    }
    if (stored->operation.operated_at > now) {
        return Result<OperationRecord>::Failure(ErrorCode::kConflict, "操作时间晚于当前时间，不能撤销");
    }
    if (!IsWithinUndoWindow(stored->operation, now)) {
        return Result<OperationRecord>::Failure(ErrorCode::kConflict, "操作已超过十五分钟撤销期限");
    }
    return Result<OperationRecord>::Success(stored->operation);
}

}  // namespace

Result<OperationRecord> AppendMockScheduleOperation(OperationRecord operation) {
    // TODO(#121)：真实存储接入后，替换为 OperationRecord Store 的原子 INSERT。
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    return Result<OperationRecord>::Success(AppendOperationLocked(store, std::move(operation), Now()));
}

Result<OperationRecord> AppendMockScheduleOperationForTesting(OperationRecord operation, DateTime operated_at) {
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    return Result<OperationRecord>::Success(AppendOperationLocked(store, std::move(operation), operated_at));
}

std::vector<OperationRecord> LoadMockScheduleOperations() {
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);

    std::vector<OperationRecord> operations;
    operations.reserve(store.operations.size());
    for (const StoredOperation& stored : store.operations) {
        if (stored.active) operations.push_back(stored.operation);
    }
    return operations;
}

Result<OperationRecord> FindUndoableMockScheduleOperation(OperationId operation_id, DateTime now) {
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    return ValidateUndoableOperation(FindOperationLocked(store, operation_id), now);
}

Result<OperationRecord> InvalidateMockScheduleOperationAndAppendUndo(OperationId operation_id,
                                                                     OperationRecord undo_operation, DateTime now) {
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);

    StoredOperation* target = FindOperationLocked(store, operation_id);
    const Result<OperationRecord> undoable = ValidateUndoableOperation(target, now);
    if (!undoable.ok()) return undoable;

    if (store.next_undo_commit_failure.has_value()) {
        Status failure = std::move(*store.next_undo_commit_failure);
        store.next_undo_commit_failure.reset();
        return Result<OperationRecord>{.status = std::move(failure), .value = std::nullopt};
    }

    // 先完成可能分配内存的追加，再失效原记录，确保追加失败不会消费目标操作。
    OperationRecord appended = AppendOperationLocked(store, std::move(undo_operation), now);
    target->active = false;
    return Result<OperationRecord>::Success(std::move(appended));
}

void FailNextMockScheduleUndoCommitForTesting(Status status) {
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.next_undo_commit_failure = std::move(status);
}

void ResetMockScheduleOperationsForTesting() {
    MockOperationStore& store = Store();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.operations.clear();
    store.next_id = 1;
    store.next_undo_commit_failure.reset();
}

}  // namespace voicelife::schedule
