#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace voicelife::timing {

/// 日程模块分配的一次性 task 标识；Timing 只比较该值，不解释其内容。
class TaskId {
   public:
    /**
     * @brief 从不透明字符串创建 task 标识。
     * @param value 上游分配的标识。
     * @return value 非空时返回标识，否则返回 nullopt。
     */
    static std::optional<TaskId> Create(std::string value);

    /**
     * @brief 返回不透明标识原值。
     * @return 注册上游提供的非空字符串。
     */
    [[nodiscard]] const std::string& Value() const;

    bool operator==(const TaskId&) const = default;

   private:
    explicit TaskId(std::string value);

    std::string value_;
};

/// 绝对墙上时钟时间点；字符串解析和时区换算由 Timing 上游负责。
using TriggerAt = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/// task 到期回调；Runner 传回注册时的标识与绝对到点时刻。
using TaskCallback = std::function<void(const TaskId&, TriggerAt)>;

/// Runner 应用注册命令后的最终结果。
enum class RegisterTaskResult {
    kRegistered,
    kDuplicate,
};

/// 单条注册命令的最终结果通知；由 Runner 消费命令时调用。
using RegisterTaskResultCallback = std::function<void(RegisterTaskResult)>;

/// Runner 应用取消命令后的最终结果。
enum class CancelTaskResult {
    kCancelled,
    kNotFound,
};

/// 单条取消命令的最终结果通知；由 Runner 消费命令时调用。
using CancelTaskResultCallback = std::function<void(CancelTaskResult)>;

/// 一次性 task 的生命周期状态。
enum class TaskStatus {
    kPending,
    kExecuting,
    kCompleted,
    kCancelled,
};

/// 不包含运行时 callback 的 task 领域数据。
struct Task {
    TaskId id;
    TriggerAt trigger_at;
    TaskStatus status = TaskStatus::kPending;
    TriggerAt created_at;
    TriggerAt updated_at;
};

/// 异步注册一个一次性 task 的命令；callback 仅在内存中绑定。
struct RegisterTaskCommand {
    TaskId task_id;
    TriggerAt trigger_at;
    TaskCallback callback;
    RegisterTaskResultCallback on_result;
};

/// 异步取消一个已注册 task 的命令。
struct CancelTaskCommand {
    TaskId task_id;
    CancelTaskResultCallback on_result;
};

/// 公开命令入口的同步接收结果；不表示命令已经被 Runner 应用。
enum class CommandAcceptance {
    kAccepted,
};

/// Runner 推进一个已确定到期批次后的可观察结果。
struct RunDueTasksResult {
    size_t processed_count = 0;
    size_t skipped_count = 0;
    std::optional<TriggerAt> next_wake_at;
};

/**
 * @brief 判断 task 生命周期是否允许从当前状态迁移到目标状态。
 * @param from 当前状态。
 * @param to 目标状态。
 * @return 文档允许该迁移时返回 true。
 */
bool CanTransition(TaskStatus from, TaskStatus to);

/// 日程模块提交一次性 task 注册和取消命令的公开接口。
/// 外部调用异步排队；Runner callback 在所属 Runner 执行上下文中调用时立即应用。
class TimingTaskService {
   public:
    /** @brief 允许通过接口指针安全释放实现。 */
    virtual ~TimingTaskService() = default;

    /**
     * @brief 提交一个一次性 task；外部调用异步排队，Runner callback 内调用立即应用。
     * @param command 包含不透明 task_id、绝对到点时刻和运行时 callback 的命令。
     * @return kAccepted 表示命令已被接收；外部调用稍后通知结果，callback 内调用可在返回前通知。
     */
    virtual CommandAcceptance RegisterTask(RegisterTaskCommand command) = 0;

    /**
     * @brief 提交一个 task 取消请求；外部调用异步排队，Runner callback 内调用立即应用。
     * @param command 包含待取消的不透明 task_id 和可选的最终结果回调。
     * @return kAccepted 表示命令已被接收；外部调用稍后通知结果，callback 内调用可在返回前通知。
     */
    virtual CommandAcceptance CancelTask(CancelTaskCommand command) = 0;
};

/**
 * @brief Host 可用的异步命令 Runner，独占内存 task 注册表。
 *
 * 外部命令入口只保存命令；Runner callback 内命令由同一 Runner 执行上下文立即应用。
 */
class InMemoryTimingTaskRunner final : public TimingTaskService {
   public:
    /**
     * @brief 接收一条注册命令；Runner callback 内调用时立即应用，其他上下文异步排队。
     * @param command 待 Runner 应用的注册命令。
     * @return 命令接收后固定返回 kAccepted。
     */
    CommandAcceptance RegisterTask(RegisterTaskCommand command) override;

    /**
     * @brief 接收一条取消命令；Runner callback 内调用时立即应用，其他上下文异步排队。
     * @param command 待 Runner 应用的取消命令。
     * @return 命令接收后固定返回 kAccepted。
     */
    CommandAcceptance CancelTask(CancelTaskCommand command) override;

    /**
     * @brief 按接收顺序应用本轮开始时已有的注册和取消命令。
     * @param applied_at 本轮写入 task 创建与更新时间的墙上时钟。
     * @return 本轮消费的命令数量。
     */
    size_t ProcessPendingCommands(TriggerAt applied_at);

    /**
     * @brief 应用已接收命令并在调用线程中串行推进 trigger_at 不晚于 now 的 task。
     * @param now 本轮到期边界和生命周期更新时间。
     * @return 本轮处理、跳过数量及剩余 task 的最近唤醒时间。
     */
    RunDueTasksResult RunDueTasks(TriggerAt now);

    /**
     * @brief 查询 pending 注册表中的最早到点时刻。
     * @return 注册表为空时返回 nullopt，否则返回最早 trigger_at。
     */
    [[nodiscard]] std::optional<TriggerAt> NextWakeAt() const;

   private:
    /// pending task 及其仅驻留内存的运行时回调。
    struct PendingTask {
        Task task;
        TaskCallback callback;
    };

    using PendingTaskList = std::list<PendingTask>;
    /// 按公开入口接收顺序保存的 Runner 命令。
    using PendingCommand = std::variant<RegisterTaskCommand, CancelTaskCommand>;

    void ApplyRegisterTask(RegisterTaskCommand command, TriggerAt applied_at);
    void ApplyCancelTask(CancelTaskCommand command, TriggerAt applied_at);

    std::deque<PendingCommand> commands_;
    std::vector<std::string> used_task_ids_;
    PendingTaskList pending_tasks_;
    std::unordered_map<std::string, PendingTaskList::iterator> pending_tasks_by_id_;
    std::vector<Task> terminal_tasks_;
    bool processing_commands_ = false;
    std::optional<TriggerAt> callback_applied_at_;
};

}  // namespace voicelife::timing
