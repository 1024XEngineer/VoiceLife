#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voicelife {

enum class ReminderStatus {
    Scheduled,
    Snoozed,
    Pushed,
    Closed,
};

struct Event {
    std::string id;
    std::string title;
    std::string starts_at;
    std::string ends_at;
    std::string kind;  // point or time_block
    std::string time_zone;
    std::string location;
    std::string notes;
    std::string recurrence_frequency;
    int recurrence_weekday = 0;
    int recurrence_month_day = 0;
    int reminder_offset_minutes = 0;
    bool weak_reminder_enabled = false;
    int weak_reminder_minutes = 15;
    bool paused = false;
    bool terminated = false;
    std::vector<std::string> skipped_occurrences;
    int64_t created_at = 0;
    int64_t updated_at = 0;
};

struct Reminder {
    std::string id;
    std::string event_id;
    std::string original_start_at;
    std::string trigger_at;
    bool weak = false;
    ReminderStatus status = ReminderStatus::Scheduled;
    int snooze_count = 0;
    int64_t delivered_at = 0;
    int64_t closed_at = 0;
    // The Gateway delivery is keyed by reminder + trigger time. Keeping the
    // last acknowledged trigger makes due notifications retryable across
    // network failures and device restarts without making the Gateway a
    // calendar database.
    std::string im_reported_trigger_at;
};

struct ShortNote {
    std::string id;
    std::string content;
    std::string category;
    int64_t expires_at = 0;
    int64_t created_at = 0;
};

struct Receipt {
    std::string id;
    std::string type;
    std::string speech;
    int64_t created_at = 0;
};

struct UndoOperation {
    std::string id;
    std::string action;
    std::string event_snapshot;
    std::string reminder_snapshot;
    std::vector<std::string> reminder_snapshots;
    int64_t expires_at = 0;
    bool undone = false;
    int64_t created_at = 0;
};

struct ProcessedImAction {
    std::string action_id;
    bool ok = false;
    std::string result_json;
    int64_t processed_at = 0;
};

struct State {
    std::vector<Event> events;
    std::vector<Reminder> reminders;
    std::vector<ShortNote> notes;
    std::vector<Receipt> receipts;
    std::vector<UndoOperation> undo_operations;
    std::vector<ProcessedImAction> processed_im_actions;
};

// Parse an ISO-8601 timestamp with a numeric offset or Z. Returns -1 on error.
int64_t ParseIso8601(const std::string& value);
std::string FormatUtc(int64_t epoch_seconds);
// Format an instant for short spoken output in the MVP's fixed
// Asia/Shanghai timezone. Seconds and transport-oriented ISO syntax are
// intentionally omitted.
std::string FormatSpokenTime(const std::string& value, int64_t now_epoch_seconds);
std::string FormatSpokenTimeRange(const std::string& starts_at, const std::string& ends_at,
                                  int64_t now_epoch_seconds);
int Iso8601LocalWeekday(const std::string& value);
int Iso8601LocalMonthDay(const std::string& value);
std::string AlignWeeklyStartAt(const std::string& proposed_start_at, int target_weekday,
                               int64_t now_epoch_seconds);
std::string NextOccurrenceUtc(const Event& event, const std::string& current_start_at);
std::string NewId(const char* prefix);
bool IsSensitiveNote(const std::string& content);
const char* ReminderStatusName(ReminderStatus status);
ReminderStatus ReminderStatusFromName(const std::string& name);

}  // namespace voicelife
