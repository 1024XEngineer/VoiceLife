#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "voicelife/timing/timing_task_service.h"

namespace voicelife::timing {
namespace {

bool IsKnownReminderType(ReminderType type) { return type == ReminderType::kWeak || type == ReminderType::kStrong; }

Status ValidateReminderRuleInput(const ReminderRuleInput& input) {
    if (!IsKnownReminderType(input.type) || input.offset_minutes > 0 || input.channel.empty() || input.source.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "提醒规则类型、时间偏移、渠道或来源无效");
    }
    if (input.type == ReminderType::kWeak && (input.max_snooze_count != 0 || input.snooze_interval_minutes != 0)) {
        return Status::Error(ErrorCode::kInvalidArgument, "弱提醒不能配置 snooze");
    }
    if (input.type == ReminderType::kStrong && (input.max_snooze_count <= 0 || input.snooze_interval_minutes <= 0)) {
        return Status::Error(ErrorCode::kInvalidArgument, "强提醒的 snooze 次数和间隔必须为正数");
    }
    return Status::Ok();
}

void SortRules(std::vector<ReminderRule>& rules) {
    std::sort(rules.begin(), rules.end(), [](const ReminderRule& left, const ReminderRule& right) {
        return left.offset_minutes != right.offset_minutes ? left.offset_minutes < right.offset_minutes
                                                           : left.id < right.id;
    });
}

}  // namespace

Result<UpsertReminderRulesResult> DefaultTimingTaskService::UpsertReminderRules(
    const UpsertReminderRulesCommand& command) {
    if (command.task_id.empty() || command.schedule_id.empty() || command.rules.empty()) {
        return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kInvalidArgument, "提醒规则缺少任务、日程或规则");
    }

    const auto task = store_.FindTask(command.task_id);
    if (!task.ok()) {
        return Result<UpsertReminderRulesResult>::Failure(task.status.code, task.status.message);
    }
    if (task.value->schedule_id != command.schedule_id) {
        return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kConflict, "提醒规则不属于指定日程");
    }
    if (task.value->status != TimingTaskStatus::kActive) {
        return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kConflict, "已终止任务不能更新提醒规则");
    }

    const auto stored_rules = store_.ListRules(command.task_id);
    if (!stored_rules.ok()) {
        return Result<UpsertReminderRulesResult>::Failure(stored_rules.status.code, stored_rules.status.message);
    }

    std::unordered_map<std::string, ReminderRule> rules_by_id;
    for (const auto& rule : *stored_rules.value) {
        rules_by_id.emplace(rule.id, rule);
    }

    std::unordered_set<std::string> requested_ids;
    std::vector<ReminderRule> changes;
    changes.reserve(command.rules.size());
    const int64_t now = clock_.Now();
    for (const auto& input : command.rules) {
        const Status validation = ValidateReminderRuleInput(input);
        if (!validation.ok()) {
            return Result<UpsertReminderRulesResult>::Failure(validation.code, validation.message);
        }
        if (!input.reminder_rule_id.empty() && !requested_ids.insert(input.reminder_rule_id).second) {
            return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kConflict, "同一请求不能重复更新提醒规则");
        }

        ReminderRule rule{};
        if (input.reminder_rule_id.empty()) {
            rule.id = ids_.NextReminderRuleId();
            if (rule.id.empty() || rules_by_id.contains(rule.id) || !requested_ids.insert(rule.id).second) {
                return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kConflict, "生成的提醒规则标识冲突");
            }
            rule.task_id = command.task_id;
            rule.created_at = now;
            rule.status = ReminderRuleStatus::kActive;
        } else {
            const auto existing = rules_by_id.find(input.reminder_rule_id);
            if (existing == rules_by_id.end()) {
                return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kNotFound, "提醒规则不存在或不属于该任务");
            }
            rule = existing->second;
        }

        rule.type = input.type;
        rule.offset_minutes = input.offset_minutes;
        rule.max_snooze_count = input.max_snooze_count;
        rule.snooze_interval_minutes = input.snooze_interval_minutes;
        rule.channel = input.channel;
        rule.source = input.source;
        rule.updated_at = now;
        rules_by_id.insert_or_assign(rule.id, rule);
        changes.push_back(std::move(rule));
    }

    std::vector<ReminderRule> result_rules;
    result_rules.reserve(rules_by_id.size());
    int active_on_time_strong_count = 0;
    for (const auto& [_, rule] : rules_by_id) {
        if (rule.status == ReminderRuleStatus::kActive && rule.type == ReminderType::kStrong &&
            rule.offset_minutes == 0) {
            ++active_on_time_strong_count;
        }
        result_rules.push_back(rule);
    }
    if (active_on_time_strong_count > 1) {
        return Result<UpsertReminderRulesResult>::Failure(ErrorCode::kConflict, "同一任务只能有一条准点强提醒规则");
    }
    SortRules(result_rules);

    const Status saved = store_.UpsertRules(command.task_id, changes);
    if (!saved.ok()) {
        return Result<UpsertReminderRulesResult>::Failure(saved.code, saved.message);
    }
    return Result<UpsertReminderRulesResult>::Success({
        .task_id = command.task_id,
        .reminder_rules = std::move(result_rules),
    });
}

}  // namespace voicelife::timing
