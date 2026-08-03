#pragma once

#include <string>

#include "voicelife/timing/timing_task_store.h"

struct sqlite3;

namespace voicelife::timing_sqlite {

class SqliteTimingTaskStore final : public timing::TimingTaskStorePort {
   public:
    explicit SqliteTimingTaskStore(std::string path) : path_(std::move(path)) {}
    ~SqliteTimingTaskStore() override;
    Status Open();
    Status RegisterTaskWithRules(const timing::TimingTask&, const std::vector<timing::ReminderRule>&) override;
    Result<timing::TimingTask> FindTask(const std::string&) override;
    Status UpdateTask(const timing::TimingTask&) override;
    Status UpdateTaskWithEvent(const timing::TimingTask&, const timing::TimingEvent&) override;
    Result<std::vector<timing::TimingTask>> ListTasks() override;
    Result<std::vector<timing::TimingTask>> ListDueTasks(int64_t) override;
    Status MaterializeOccurrence(const timing::TimerInstance&, const std::vector<timing::ReminderTrigger>&,
                                 const timing::TimingTask&, const timing::TimingEvent&) override;
    Status UpsertInstance(const timing::TimerInstance&) override;
    Result<std::optional<timing::TimerInstance>> FindInstance(const std::string&) override;
    Result<std::optional<timing::TimerInstance>> FindInstanceByOccurrence(const std::string&, int64_t) override;
    Result<std::vector<timing::TimerInstance>> ListInstances(const std::string&) override;
    Result<int> ApplyFutureUpdate(const timing::TimingTask&, int64_t, int64_t, const timing::TimingEvent&) override;
    Result<int> CancelFuture(const timing::TimingTask&, int64_t, int64_t, const timing::TimingEvent&) override;
    Status UpsertRules(const std::string&, const std::vector<timing::ReminderRule>&) override;
    Result<int> DisableRuleAndCancelPendingTriggers(const std::string&, int64_t) override;
    Result<std::optional<timing::ReminderRule>> FindRule(const std::string&) override;
    Result<std::vector<timing::ReminderRule>> ListRules(const std::string&) override;
    Status UpsertTriggers(const std::vector<timing::ReminderTrigger>&) override;
    Status UpdateTrigger(const timing::ReminderTrigger&) override;
    Status UpdateTriggerWithEvent(const timing::ReminderTrigger&, const timing::TimingEvent&) override;
    Result<std::optional<timing::ReminderTrigger>> FindTrigger(const std::string&) override;
    Result<std::vector<timing::ReminderTrigger>> ListTriggers() override;
    Result<std::vector<timing::ReminderTrigger>> ListDueTriggers(int64_t) override;
    Status EnqueueEvent(const timing::TimingEvent&) override;
    Result<std::vector<timing::TimingEvent>> ListPendingEvents() override;
    Status MarkEventPublished(const std::string&) override;

   private:
    std::string path_;
    sqlite3* db_ = nullptr;
};

}  // namespace voicelife::timing_sqlite
