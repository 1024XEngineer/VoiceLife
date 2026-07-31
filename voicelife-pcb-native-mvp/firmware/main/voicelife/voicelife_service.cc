#include "voicelife_service.h"

#include <esp_log.h>
#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace voicelife {
namespace {

constexpr char kTag[] = "VoiceLife";
constexpr int64_t kUndoWindowSeconds = 10 * 60;
constexpr int64_t kNoteLifetimeSeconds = 24 * 60 * 60;
constexpr size_t kCalendarQueryReplyLimit = 2;
constexpr size_t kCalendarFindReplyLimit = 3;
constexpr char kCalendarQueryImTail[] = "其余日程我就不逐条念了，详细信息可在 IM 中查看。";

const char* StringArg(const cJSON* object, const char* key) {
    const cJSON* value =
        object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : nullptr;
}

bool BoolArg(const cJSON* object, const char* key, bool fallback = false) {
    const cJSON* value =
        object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}

int IntArg(const cJSON* object, const char* key, int fallback = 0) {
    const cJSON* value =
        object == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

void AddString(cJSON* object, const char* key, const std::string& value) {
    if (!value.empty())
        cJSON_AddStringToObject(object, key, value.c_str());
}

void AddNullableString(cJSON* object, const char* key, const std::string& value) {
    if (value.empty())
        cJSON_AddNullToObject(object, key);
    else
        cJSON_AddStringToObject(object, key, value.c_str());
}

cJSON* CalendarFindCandidateJson(const Event& event, const std::string& original_start_at) {
    std::string starts_at = original_start_at.empty() ? event.starts_at : original_start_at;
    std::string ends_at = event.ends_at;
    if (!original_start_at.empty() && !event.ends_at.empty()) {
        const int64_t event_start = ParseIso8601(event.starts_at);
        const int64_t event_end = ParseIso8601(event.ends_at);
        const int64_t occurrence_start = ParseIso8601(original_start_at);
        if (event_start >= 0 && event_end > event_start && occurrence_start >= 0) {
            ends_at = FormatUtc(occurrence_start + event_end - event_start);
        }
    }

    cJSON* object = cJSON_CreateObject();
    AddString(object, "eventId", event.id);
    AddString(object, "title", event.title);
    AddString(object, "startsAt", starts_at);
    AddString(object, "endsAt", ends_at);
    AddString(object, "originalStartAt", starts_at);
    cJSON_AddBoolToObject(object, "recurring", !event.recurrence_frequency.empty());
    return object;
}

std::string PrintJson(cJSON* object) {
    if (object == nullptr)
        return {};
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text == nullptr ? std::string{} : std::string(text);
    if (text != nullptr)
        cJSON_free(text);
    cJSON_Delete(object);
    return result;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    auto normalize = [](const std::string& value) {
        std::string normalized;
        normalized.reserve(value.size());
        for (unsigned char byte : value) {
            if (byte < 0x80) {
                if (std::isspace(byte) || byte == '-' || byte == '_') continue;
                normalized.push_back(static_cast<char>(std::tolower(byte)));
            } else {
                normalized.push_back(static_cast<char>(byte));
            }
        }
        return normalized;
    };
    const std::string normalized_needle = normalize(needle);
    if (normalized_needle.empty()) return true;
    return normalize(haystack).find(normalized_needle) != std::string::npos;
}

size_t EditDistance(const std::string& left, const std::string& right) {
    std::vector<size_t> previous(right.size() + 1);
    std::vector<size_t> current(right.size() + 1);
    for (size_t index = 0; index <= right.size(); ++index) previous[index] = index;
    for (size_t left_index = 1; left_index <= left.size(); ++left_index) {
        current[0] = left_index;
        for (size_t right_index = 1; right_index <= right.size(); ++right_index) {
            const size_t substitution =
                previous[right_index - 1] + (left[left_index - 1] == right[right_index - 1] ? 0 : 1);
            current[right_index] = std::min(
                {previous[right_index] + 1, current[right_index - 1] + 1, substitution});
        }
        previous.swap(current);
    }
    return previous[right.size()];
}

bool RangesOverlap(int64_t start_a, int64_t end_a, int64_t start_b, int64_t end_b,
                   bool block_a, bool block_b) {
    if (!block_a && !block_b) return start_a == start_b;
    if (!block_a) return start_a >= start_b && start_a < end_b;
    if (!block_b) return start_b >= start_a && start_b < end_a;
    return start_a < end_b && start_b < end_a;
}

bool SameInstant(const std::string& first, const std::string& second) {
    if (first.empty() || second.empty()) return first.empty() && second.empty();
    const int64_t first_epoch = ParseIso8601(first);
    const int64_t second_epoch = ParseIso8601(second);
    return first_epoch >= 0 && second_epoch >= 0 && first_epoch == second_epoch;
}

bool SameEventDefinition(const Event& first, const Event& second) {
    return first.title == second.title && SameInstant(first.starts_at, second.starts_at) &&
           SameInstant(first.ends_at, second.ends_at) && first.kind == second.kind &&
           first.time_zone == second.time_zone && first.location == second.location &&
           first.notes == second.notes &&
           first.recurrence_frequency == second.recurrence_frequency &&
           first.recurrence_weekday == second.recurrence_weekday &&
           first.recurrence_month_day == second.recurrence_month_day &&
           first.reminder_offset_minutes == second.reminder_offset_minutes &&
           first.weak_reminder_enabled == second.weak_reminder_enabled &&
           first.weak_reminder_minutes == second.weak_reminder_minutes;
}

std::string CanonicalEvent(const Event& event) {
    return event.title + "|" + event.starts_at + "|" + event.ends_at + "|" + event.kind;
}

std::string FnvToken(const std::string& input) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : input) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "vl-%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

Event EventFromSnapshot(const std::string& snapshot) {
    Event event;
    cJSON* object = cJSON_Parse(snapshot.c_str());
    if (object == nullptr) return event;
    auto read = [object](const char* key) -> std::string {
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
        return cJSON_IsString(value) && value->valuestring ? value->valuestring : std::string{};
    };
    event.id = read("id");
    event.title = read("title");
    event.starts_at = read("startsAt");
    event.ends_at = read("endsAt");
    event.kind = read("kind");
    event.time_zone = read("timeZone");
    event.location = read("location");
    event.notes = read("notes");
    event.recurrence_frequency = read("recurrenceFrequency");
    const cJSON* value = nullptr;
    const cJSON* recurrence = cJSON_GetObjectItemCaseSensitive(object, "recurrence");
    if (cJSON_IsObject(recurrence)) {
        const cJSON* frequency = cJSON_GetObjectItemCaseSensitive(recurrence, "frequency");
        if (event.recurrence_frequency.empty() && cJSON_IsString(frequency) && frequency->valuestring != nullptr) {
            event.recurrence_frequency = frequency->valuestring;
        }
        value = cJSON_GetObjectItemCaseSensitive(recurrence, "weekday");
        event.recurrence_weekday = cJSON_IsNumber(value) ? value->valueint : 0;
        value = cJSON_GetObjectItemCaseSensitive(recurrence, "monthDay");
        event.recurrence_month_day = cJSON_IsNumber(value) ? value->valueint : 0;
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "recurrenceWeekday");
    if (cJSON_IsNumber(value)) event.recurrence_weekday = value->valueint;
    value = cJSON_GetObjectItemCaseSensitive(object, "recurrenceMonthDay");
    if (cJSON_IsNumber(value)) event.recurrence_month_day = value->valueint;
    value = cJSON_GetObjectItemCaseSensitive(object, "reminderOffsetMinutes");
    event.reminder_offset_minutes = cJSON_IsNumber(value) ? value->valueint : 0;
    value = cJSON_GetObjectItemCaseSensitive(object, "weakReminderEnabled");
    event.weak_reminder_enabled = cJSON_IsTrue(value);
    value = cJSON_GetObjectItemCaseSensitive(object, "weakReminderMinutes");
    event.weak_reminder_minutes = cJSON_IsNumber(value) ? value->valueint : 15;
    value = cJSON_GetObjectItemCaseSensitive(object, "paused");
    event.paused = cJSON_IsTrue(value);
    value = cJSON_GetObjectItemCaseSensitive(object, "terminated");
    event.terminated = cJSON_IsTrue(value);
    value = cJSON_GetObjectItemCaseSensitive(object, "skippedOccurrences");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, value) {
        if (cJSON_IsString(item) && item->valuestring != nullptr) event.skipped_occurrences.emplace_back(item->valuestring);
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "createdAt");
    event.created_at = cJSON_IsNumber(value) ? static_cast<int64_t>(value->valuedouble) : 0;
    value = cJSON_GetObjectItemCaseSensitive(object, "updatedAt");
    event.updated_at = cJSON_IsNumber(value) ? static_cast<int64_t>(value->valuedouble) : event.created_at;
    cJSON_Delete(object);
    return event;
}

Reminder ReminderFromSnapshot(const std::string& snapshot) {
    Reminder reminder;
    cJSON* object = cJSON_Parse(snapshot.c_str());
    if (object == nullptr) return reminder;
    auto read = [object](const char* key) -> std::string {
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
        return cJSON_IsString(value) && value->valuestring ? value->valuestring : std::string{};
    };
    reminder.id = read("id");
    if (reminder.id.empty()) reminder.id = read("reminderId");
    reminder.event_id = read("eventId");
    reminder.original_start_at = read("originalStartAt");
    reminder.trigger_at = read("triggerAt");
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, "weak");
    reminder.weak = cJSON_IsTrue(value);
    reminder.status = ReminderStatusFromName(read("status"));
    value = cJSON_GetObjectItemCaseSensitive(object, "snoozeCount");
    reminder.snooze_count = cJSON_IsNumber(value) ? value->valueint : 0;
    value = cJSON_GetObjectItemCaseSensitive(object, "deliveredAt");
    reminder.delivered_at = cJSON_IsNumber(value) ? static_cast<int64_t>(value->valuedouble) : 0;
    value = cJSON_GetObjectItemCaseSensitive(object, "closedAt");
    reminder.closed_at = cJSON_IsNumber(value) ? static_cast<int64_t>(value->valuedouble) : 0;
    reminder.im_reported_trigger_at = read("imReportedTriggerAt");
    cJSON_Delete(object);
    return reminder;
}

}  // namespace

VoiceLifeService::VoiceLifeService(Storage* storage, Clock clock)
    : storage_(storage == nullptr ? &flash_storage_ : storage),
      clock_(clock == nullptr ? []() { return static_cast<int64_t>(std::time(nullptr)); } : std::move(clock)) {}

int64_t VoiceLifeService::Now() const {
    return clock_ == nullptr ? static_cast<int64_t>(std::time(nullptr)) : clock_();
}

bool VoiceLifeService::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;
    if (!storage_->Initialize()) {
        ESP_LOGE(kTag, "Storage initialization failed; refusing to run business tools");
        return false;
    }
    if (!storage_->Load(&state_)) {
        ESP_LOGW(kTag, "Storage journal was invalid; starting with an empty state");
        state_ = State{};
        storage_->Save(state_);
    }
    initialized_ = true;
    return true;
}

void VoiceLifeService::SetSpeechCallback(SpeechCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    speech_callback_ = std::move(callback);
}

cJSON* VoiceLifeService::Error(const std::string& message) const {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "message", message.c_str());
    cJSON_AddStringToObject(result, "speech", message.c_str());
    return result;
}

cJSON* VoiceLifeService::Result(const std::string& speech) const {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "speech", speech.c_str());
    return result;
}

bool VoiceLifeService::SaveLocked() {
    if (storage_->Save(state_)) return true;

    State persisted;
    if (storage_->Load(&persisted)) {
        state_ = std::move(persisted);
    } else {
        ESP_LOGE(kTag, "Failed to reload the last persisted state after a commit error");
    }
    return false;
}

cJSON* VoiceLifeService::CommitLocked(cJSON* success_result, const char* operation) {
    if (SaveLocked()) return success_result;

    ESP_LOGE(kTag, "Failed to persist %s", operation == nullptr ? "operation" : operation);
    cJSON_Delete(success_result);
    cJSON* result = Error("本地保存失败，请重试");
    cJSON_AddStringToObject(result, "reason", "storage_commit_failed");
    return result;
}

Event* VoiceLifeService::FindEventLocked(const std::string& event_id) {
    auto it = std::find_if(state_.events.begin(), state_.events.end(),
                           [&event_id](const Event& event) { return event.id == event_id; });
    return it == state_.events.end() ? nullptr : &*it;
}

const Event* VoiceLifeService::FindEventLocked(const std::string& event_id) const {
    auto it = std::find_if(state_.events.begin(), state_.events.end(),
                           [&event_id](const Event& event) { return event.id == event_id; });
    return it == state_.events.end() ? nullptr : &*it;
}

const Event* VoiceLifeService::FindEquivalentEventLocked(const Event& candidate) const {
    auto it = std::find_if(state_.events.begin(), state_.events.end(),
                           [&candidate](const Event& existing) {
                               return !existing.terminated &&
                                      SameEventDefinition(existing, candidate);
                           });
    return it == state_.events.end() ? nullptr : &*it;
}

Reminder* VoiceLifeService::FindReminderLocked(const std::string& reminder_id) {
    auto it = std::find_if(state_.reminders.begin(), state_.reminders.end(),
                           [&reminder_id](const Reminder& reminder) { return reminder.id == reminder_id; });
    return it == state_.reminders.end() ? nullptr : &*it;
}

const Reminder* VoiceLifeService::FindReminderLocked(const std::string& reminder_id) const {
    auto it = std::find_if(state_.reminders.begin(), state_.reminders.end(),
                           [&reminder_id](const Reminder& reminder) { return reminder.id == reminder_id; });
    return it == state_.reminders.end() ? nullptr : &*it;
}

cJSON* VoiceLifeService::EventToPublicJson(const Event& event) const {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "eventId", event.id);
    AddString(object, "id", event.id);
    AddString(object, "title", event.title);
    AddString(object, "startsAt", event.starts_at);
    AddNullableString(object, "endsAt", event.ends_at);
    AddString(object, "kind", event.kind);
    AddString(object, "timeZone", event.time_zone);
    AddNullableString(object, "location", event.location);
    AddNullableString(object, "notes", event.notes);
    cJSON_AddBoolToObject(object, "weakReminderEnabled", event.weak_reminder_enabled);
    cJSON_AddNumberToObject(object, "weakReminderMinutes", event.weak_reminder_minutes);
    cJSON_AddNumberToObject(object, "reminderOffsetMinutes", event.reminder_offset_minutes);
    if (!event.recurrence_frequency.empty()) {
        cJSON* recurrence = cJSON_AddObjectToObject(object, "recurrence");
        AddString(recurrence, "frequency", event.recurrence_frequency);
        if (event.recurrence_weekday > 0) cJSON_AddNumberToObject(recurrence, "weekday", event.recurrence_weekday);
        if (event.recurrence_month_day > 0) cJSON_AddNumberToObject(recurrence, "monthDay", event.recurrence_month_day);
    } else {
        cJSON_AddNullToObject(object, "recurrence");
    }
    cJSON* skipped = cJSON_AddArrayToObject(object, "skippedOccurrences");
    for (const auto& occurrence : event.skipped_occurrences) {
        cJSON_AddItemToArray(skipped, cJSON_CreateString(occurrence.c_str()));
    }
    cJSON_AddBoolToObject(object, "paused", event.paused);
    cJSON_AddBoolToObject(object, "terminated", event.terminated);
    return object;
}

cJSON* VoiceLifeService::ReminderToPublicJson(const Reminder& reminder, const Event* event) const {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "reminderId", reminder.id);
    AddString(object, "eventId", reminder.event_id);
    AddString(object, "originalStartAt", reminder.original_start_at);
    AddString(object, "triggerAt", reminder.trigger_at);
    AddString(object, "status", ReminderStatusName(reminder.status));
    cJSON_AddBoolToObject(object, "weak", reminder.weak);
    cJSON_AddNumberToObject(object, "snoozeCount", reminder.snooze_count);
    cJSON_AddNumberToObject(object, "deliveredAt", static_cast<double>(reminder.delivered_at));
    cJSON_AddNumberToObject(object, "closedAt", static_cast<double>(reminder.closed_at));
    if (event != nullptr) {
        Event occurrence = *event;
        const int64_t occurrence_start = ParseIso8601(reminder.original_start_at);
        const int64_t event_start = ParseIso8601(event->starts_at);
        if (occurrence_start >= 0 && event_start >= 0) {
            occurrence.starts_at = FormatUtc(occurrence_start);
            if (!event->ends_at.empty()) {
                const int64_t event_end = ParseIso8601(event->ends_at);
                if (event_end > event_start) {
                    occurrence.ends_at = FormatUtc(occurrence_start + event_end - event_start);
                }
            }
        }
        AddString(object, "title", event->title);
        AddString(object, "startsAt", occurrence.starts_at);
        AddNullableString(object, "endsAt", occurrence.ends_at);
        AddNullableString(object, "location", occurrence.location);
        AddNullableString(object, "notes", occurrence.notes);
    }
    return object;
}

cJSON* VoiceLifeService::OccurrenceToPublicJson(const Event& event, const std::string& original_start_at) const {
    Event occurrence = event;
    const int64_t original_start = ParseIso8601(original_start_at);
    const int64_t event_start = ParseIso8601(event.starts_at);
    if (original_start >= 0 && event_start >= 0) {
        occurrence.starts_at = FormatUtc(original_start);
        if (!event.ends_at.empty()) {
            const int64_t event_end = ParseIso8601(event.ends_at);
            if (event_end > event_start) occurrence.ends_at = FormatUtc(original_start + event_end - event_start);
        }
    }
    cJSON* object = EventToPublicJson(occurrence);
    cJSON_AddStringToObject(object, "originalStartAt", original_start_at.c_str());
    return object;
}

cJSON* VoiceLifeService::OccurrenceToQueryJson(const Event& event,
                                               const std::string& original_start_at) const {
    const int64_t occurrence_start = ParseIso8601(original_start_at);
    const int64_t event_start = ParseIso8601(event.starts_at);
    std::string starts_at = occurrence_start >= 0 ? FormatUtc(occurrence_start) : event.starts_at;
    std::string ends_at;
    if (!event.ends_at.empty() && occurrence_start >= 0 && event_start >= 0) {
        const int64_t event_end = ParseIso8601(event.ends_at);
        if (event_end > event_start) ends_at = FormatUtc(occurrence_start + event_end - event_start);
    }

    cJSON* object = cJSON_CreateObject();
    AddString(object, "eventId", event.id);
    AddString(object, "title", event.title);
    AddString(object, "startsAt", starts_at);
    AddNullableString(object, "endsAt", ends_at);
    AddString(object, "originalStartAt", original_start_at);
    AddString(object, "spokenTime", FormatSpokenTimeRange(starts_at, ends_at, Now()));
    AddString(object, "location", event.location);
    return object;
}

bool VoiceLifeService::IsOccurrenceSkippedLocked(const Event& event, const std::string& occurrence_start_at) const {
    const int64_t target = ParseIso8601(occurrence_start_at);
    if (target < 0) return false;
    return std::any_of(event.skipped_occurrences.begin(), event.skipped_occurrences.end(),
                       [target](const std::string& skipped) { return ParseIso8601(skipped) == target; });
}

bool VoiceLifeService::IsOccurrenceInSeriesLocked(const Event& event, const std::string& occurrence_start_at) const {
    const int64_t target = ParseIso8601(occurrence_start_at);
    if (target < 0 || ParseIso8601(event.starts_at) < 0) return false;
    std::string cursor = FormatUtc(ParseIso8601(event.starts_at));
    for (int attempt = 0; attempt < 5000 && !cursor.empty(); ++attempt) {
        if (ParseIso8601(cursor) == target) return true;
        if (event.recurrence_frequency.empty() || ParseIso8601(cursor) > target) break;
        cursor = NextOccurrenceUtc(event, cursor);
    }
    return false;
}

void VoiceLifeService::EnumerateOccurrencesLocked(const Event& event, int64_t range_start,
                                                  int64_t range_end,
                                                  std::vector<std::string>* occurrences) const {
    if (occurrences == nullptr || range_end <= range_start || event.terminated) return;
    const int64_t first = ParseIso8601(event.starts_at);
    if (first < 0) return;
    std::string cursor = FormatUtc(first);
    for (int attempt = 0; attempt < 5000 && !cursor.empty(); ++attempt) {
        const int64_t epoch = ParseIso8601(cursor);
        if (epoch < 0 || epoch >= range_end) break;
        if (epoch >= range_start && !IsOccurrenceSkippedLocked(event, cursor)) {
            occurrences->push_back(cursor);
        }
        if (event.recurrence_frequency.empty()) break;
        const std::string next = NextOccurrenceUtc(event, cursor);
        if (next.empty() || ParseIso8601(next) <= epoch) break;
        cursor = next;
    }
}

void VoiceLifeService::EnsureRemindersForOccurrenceLocked(const Event& event,
                                                           const std::string& original_start_at,
                                                           bool only_if_future) {
    const int64_t start = ParseIso8601(original_start_at);
    if (start < 0 || IsOccurrenceSkippedLocked(event, original_start_at)) return;
    const int64_t now = Now();
    const int64_t trigger = start - event.reminder_offset_minutes * 60LL;
    if (only_if_future && trigger <= now) return;

    const bool has_main = std::any_of(state_.reminders.begin(), state_.reminders.end(),
                                      [&event, &original_start_at](const Reminder& reminder) {
                                          return !reminder.weak && reminder.event_id == event.id &&
                                                 ParseIso8601(reminder.original_start_at) == ParseIso8601(original_start_at);
                                      });
    if (!has_main) {
        Reminder reminder;
        reminder.id = NewId("reminder");
        reminder.event_id = event.id;
        reminder.original_start_at = original_start_at;
        reminder.trigger_at = FormatUtc(trigger);
        state_.reminders.push_back(reminder);
    }
    if (!event.weak_reminder_enabled) return;
    const int64_t weak_trigger = start - event.weak_reminder_minutes * 60LL;
    if (only_if_future && weak_trigger <= now) return;
    const bool has_weak = std::any_of(state_.reminders.begin(), state_.reminders.end(),
                                      [&event, &original_start_at](const Reminder& reminder) {
                                          return reminder.weak && reminder.event_id == event.id &&
                                                 ParseIso8601(reminder.original_start_at) == ParseIso8601(original_start_at);
                                      });
    if (!has_weak) {
        Reminder weak;
        weak.id = NewId("reminder");
        weak.event_id = event.id;
        weak.original_start_at = original_start_at;
        weak.trigger_at = FormatUtc(weak_trigger);
        weak.weak = true;
        state_.reminders.push_back(weak);
    }
}

void VoiceLifeService::EnsureNextReminderLocked(const Event& event, const std::string& after_original_start_at) {
    if (event.recurrence_frequency.empty() || event.terminated) return;
    std::string next = NextOccurrenceUtc(event, after_original_start_at);
    for (int attempt = 0; attempt < 5000 && !next.empty(); ++attempt) {
        if (!IsOccurrenceSkippedLocked(event, next)) {
            const int64_t trigger = ParseIso8601(next) - event.reminder_offset_minutes * 60LL;
            if (trigger > Now()) {
                EnsureRemindersForOccurrenceLocked(event, next, true);
                return;
            }
        }
        next = NextOccurrenceUtc(event, next);
    }
}

std::string VoiceLifeService::EventJson(const Event& event) const {
    return PrintJson(EventToPublicJson(event));
}

std::string VoiceLifeService::ReminderJson(const Reminder& reminder) const {
    cJSON* object = ReminderToPublicJson(reminder, FindEventLocked(reminder.event_id));
    AddString(object, "imReportedTriggerAt", reminder.im_reported_trigger_at);
    return PrintJson(object);
}

void VoiceLifeService::AddReceiptLocked(const char* type, const std::string& speech, cJSON* result) {
    Receipt receipt;
    receipt.id = NewId("receipt");
    receipt.type = type == nullptr ? "operation" : type;
    receipt.speech = speech;
    receipt.created_at = Now();
    state_.receipts.push_back(receipt);
    while (state_.receipts.size() > 64) state_.receipts.erase(state_.receipts.begin());
    if (result != nullptr) cJSON_AddStringToObject(result, "receiptId", receipt.id.c_str());
}

void VoiceLifeService::AddUndoLocked(const char* action, const Event* event, const Reminder* reminder,
                                     cJSON* result, const std::vector<Reminder>* reminders) {
    UndoOperation operation;
    operation.id = NewId("undo");
    operation.action = action == nullptr ? "operation" : action;
    operation.event_snapshot = event == nullptr ? std::string{} : EventJson(*event);
    operation.reminder_snapshot = reminder == nullptr ? std::string{} : ReminderJson(*reminder);
    if (reminders != nullptr) {
        operation.reminder_snapshots.reserve(reminders->size());
        for (const auto& item : *reminders) operation.reminder_snapshots.push_back(ReminderJson(item));
        if (operation.reminder_snapshot.empty() && !operation.reminder_snapshots.empty()) {
            operation.reminder_snapshot = operation.reminder_snapshots.front();
        }
    } else if (!operation.reminder_snapshot.empty()) {
        operation.reminder_snapshots.push_back(operation.reminder_snapshot);
    }
    operation.created_at = Now();
    operation.expires_at = operation.created_at + kUndoWindowSeconds;
    state_.undo_operations.push_back(operation);
    while (state_.undo_operations.size() > 32) state_.undo_operations.erase(state_.undo_operations.begin());
    if (result != nullptr) cJSON_AddStringToObject(result, "undoOperationId", operation.id.c_str());
}

bool VoiceLifeService::HasConflictLocked(const Event& candidate, std::vector<const Event*>* conflicts) const {
    const int64_t candidate_start = ParseIso8601(candidate.starts_at);
    const int64_t candidate_end_base = candidate.kind == "time_block" && !candidate.ends_at.empty()
        ? ParseIso8601(candidate.ends_at)
        : candidate_start + 1;
    if (candidate_start < 0 || candidate_end_base <= candidate_start) return false;

    // A recurring event is an infinite series. The MVP checks a bounded
    // near-future window, which is enough to catch practical calendar
    // collisions without materializing unbounded state on the ESP32.
    constexpr int64_t kConflictHorizonSeconds = 370LL * 24 * 60 * 60;
    const int64_t window_end = candidate_start + kConflictHorizonSeconds;
    const int64_t candidate_duration = candidate_end_base - candidate_start;
    bool found = false;
    for (const auto& existing : state_.events) {
        if (existing.id == candidate.id || existing.paused || existing.terminated) continue;
        const int64_t existing_start = ParseIso8601(existing.starts_at);
        if (existing_start < 0) continue;
        const int64_t existing_end_base = existing.kind == "time_block" && !existing.ends_at.empty()
            ? ParseIso8601(existing.ends_at)
            : existing_start + 1;
        if (existing_end_base <= existing_start) continue;

        const int64_t window_start = std::min(candidate_start, existing_start);
        std::vector<std::string> candidate_occurrences;
        std::vector<std::string> existing_occurrences;
        EnumerateOccurrencesLocked(candidate, window_start, window_end, &candidate_occurrences);
        EnumerateOccurrencesLocked(existing, window_start, window_end, &existing_occurrences);
        bool event_conflicts = false;
        for (const auto& candidate_occurrence : candidate_occurrences) {
            const int64_t candidate_epoch = ParseIso8601(candidate_occurrence);
            const bool candidate_block = candidate_duration > 1;
            const int64_t candidate_end = candidate_epoch + candidate_duration;
            for (const auto& existing_occurrence : existing_occurrences) {
                const int64_t existing_epoch = ParseIso8601(existing_occurrence);
                const int64_t existing_duration = existing_end_base - existing_start;
                const bool existing_block = existing_duration > 1;
                if (RangesOverlap(candidate_epoch, candidate_end, existing_epoch,
                                  existing_epoch + existing_duration, candidate_block, existing_block)) {
                    event_conflicts = true;
                    break;
                }
            }
            if (event_conflicts) break;
        }
        if (event_conflicts) {
            found = true;
            if (conflicts != nullptr) conflicts->push_back(&existing);
        }
    }
    return found;
}

std::string VoiceLifeService::ConflictToken(const Event& candidate,
                                            const std::vector<const Event*>& conflicts) const {
    std::string value = CanonicalEvent(candidate);
    for (const Event* conflict : conflicts) value += "|" + conflict->id;
    return FnvToken(value);
}

void VoiceLifeService::PruneLocked(int64_t now) {
    state_.notes.erase(std::remove_if(state_.notes.begin(), state_.notes.end(),
                                      [now](const ShortNote& note) { return note.expires_at <= now; }),
                       state_.notes.end());
    state_.undo_operations.erase(std::remove_if(state_.undo_operations.begin(), state_.undo_operations.end(),
                                                [now](const UndoOperation& op) {
                                                    return op.expires_at <= now && op.undone;
                                                }),
                                 state_.undo_operations.end());
}

cJSON* VoiceLifeService::CalendarCreate(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return Error("日程服务尚未初始化");
    const char* title = StringArg(arguments, "title");
    const char* starts_at = StringArg(arguments, "startsAt");
    const cJSON* delay_value = cJSON_GetObjectItemCaseSensitive(arguments, "delayMinutes");
    const bool has_delay = delay_value != nullptr;
    if (title == nullptr || *title == '\0' || (starts_at == nullptr && !has_delay))
        return Error("需要标题以及开始时间或延迟分钟数");
    if (starts_at != nullptr && has_delay) return Error("开始时间和延迟分钟数只能填写一个");
    if (has_delay && (!cJSON_IsNumber(delay_value) || delay_value->valuedouble < 1 ||
                      delay_value->valuedouble > 1440 ||
                      std::floor(delay_value->valuedouble) != delay_value->valuedouble)) {
        return Error("延迟分钟数必须是 1 到 1440 的整数");
    }
    int64_t start_epoch =
        has_delay ? Now() + delay_value->valueint * 60LL : ParseIso8601(starts_at);
    if (start_epoch < 0) return Error("开始时间必须是带时区的 ISO 8601 时间");
    int64_t start_shift_seconds = 0;

    Event event;
    event.id = NewId("event");
    event.title = title;
    event.starts_at = has_delay ? FormatUtc(start_epoch) : starts_at;
    const cJSON* ends_value = cJSON_GetObjectItemCaseSensitive(arguments, "endsAt");
    const cJSON* duration_value = cJSON_GetObjectItemCaseSensitive(arguments, "durationMinutes");
    const bool has_ends = cJSON_IsString(ends_value) && ends_value->valuestring != nullptr;
    const bool has_duration = duration_value != nullptr;
    if (has_ends && has_duration) return Error("结束时间和持续时长只能填写一个");
    if (has_duration && (!cJSON_IsNumber(duration_value) ||
                         duration_value->valuedouble < 1 ||
                         std::floor(duration_value->valuedouble) != duration_value->valuedouble)) {
        return Error("持续时长必须是正整数分钟");
    }
    const char* requested_kind = StringArg(arguments, "kind");
    event.kind = requested_kind == nullptr ? (has_ends || has_duration ? "time_block" : "point") : requested_kind;
    if (event.kind != "point" && event.kind != "time_block") return Error("kind 只能是 point 或 time_block");
    event.time_zone = StringArg(arguments, "timeZone") == nullptr ? "Asia/Shanghai" : StringArg(arguments, "timeZone");
    if (has_ends) event.ends_at = ends_value->valuestring;
    const int duration = has_duration ? duration_value->valueint : 0;
    if (event.ends_at.empty() && duration > 0) event.ends_at = FormatUtc(start_epoch + duration * 60LL);
    if (event.kind == "time_block" && event.ends_at.empty()) return Error("时间段日程需要 endsAt 或 durationMinutes");
    if (event.kind == "point" && !event.ends_at.empty()) return Error("时间点日程不能设置结束时间");
    if (!event.ends_at.empty() && (ParseIso8601(event.ends_at) <= start_epoch)) return Error("结束时间必须晚于开始时间");
    if (const char* value = StringArg(arguments, "location")) event.location = value;
    if (const char* value = StringArg(arguments, "notes")) event.notes = value;
    const cJSON* recurrence = cJSON_GetObjectItemCaseSensitive(arguments, "recurrence");
    if (cJSON_IsObject(recurrence)) {
        if (const char* value = StringArg(recurrence, "frequency")) event.recurrence_frequency = value;
        event.recurrence_weekday = IntArg(recurrence, "weekday");
        event.recurrence_month_day = IntArg(recurrence, "monthDay");
        if (event.recurrence_frequency != "daily" && event.recurrence_frequency != "weekly" && event.recurrence_frequency != "monthly") {
            return Error("周期只支持 daily、weekly 或 monthly");
        }
        if (event.recurrence_frequency == "weekly") {
            const bool weekday_explicit = cJSON_HasObjectItem(recurrence, "weekday");
            if (event.recurrence_weekday == 0) event.recurrence_weekday = Iso8601LocalWeekday(event.starts_at);
            if (event.recurrence_weekday < 1 || event.recurrence_weekday > 7) {
                return Error("每周星期必须在 1 到 7 之间");
            }
            if (weekday_explicit) {
                const std::string aligned =
                    AlignWeeklyStartAt(event.starts_at, event.recurrence_weekday, Now());
                const int64_t aligned_epoch = ParseIso8601(aligned);
                if (aligned_epoch < 0) return Error("无法计算每周日程的首次发生时间");
                start_shift_seconds = aligned_epoch - start_epoch;
                if (start_shift_seconds != 0) {
                    event.starts_at = aligned;
                    start_epoch = aligned_epoch;
                    if (!event.ends_at.empty()) {
                        const int64_t shifted_end = ParseIso8601(event.ends_at) + start_shift_seconds;
                        event.ends_at = FormatUtc(shifted_end);
                    }
                }
            }
        }
        if (event.recurrence_frequency == "monthly") {
            if (event.recurrence_month_day == 0) event.recurrence_month_day = Iso8601LocalMonthDay(event.starts_at);
            if (event.recurrence_month_day < 1 || event.recurrence_month_day > 31 ||
                event.recurrence_month_day != Iso8601LocalMonthDay(event.starts_at)) {
                return Error("首次发生日期与每月日期设置不一致");
            }
        }
    }
    if (start_epoch < Now() - 1) return Error("日程时间已经过去，请明确是今天还是明天");
    const cJSON* weak_minutes_value = cJSON_GetObjectItemCaseSensitive(arguments, "weakReminderMinutes");
    if (weak_minutes_value != nullptr && (!cJSON_IsNumber(weak_minutes_value) || weak_minutes_value->valueint != 15)) {
        return Error("MVP 的提前弱提醒固定为 15 分钟");
    }
    event.weak_reminder_enabled = BoolArg(arguments, "weakReminder", event.kind == "time_block");
    event.weak_reminder_minutes = 15;
    const char* remind_at = StringArg(arguments, "remindAt");
    int64_t remind_epoch = remind_at == nullptr ? start_epoch : ParseIso8601(remind_at);
    if (remind_at != nullptr && remind_epoch >= 0) remind_epoch += start_shift_seconds;
    if (remind_epoch < 0) return Error("提醒时间格式无效");
    if (remind_epoch > start_epoch) return Error("提醒时间不能晚于日程发生时间");
    if (remind_epoch < Now() - 1) return Error("提醒时间已经过去");
    event.reminder_offset_minutes = static_cast<int>((start_epoch - remind_epoch + 30) / 60);
    event.created_at = Now();
    event.updated_at = event.created_at;

    if (const Event* existing = FindEquivalentEventLocked(event); existing != nullptr) {
        cJSON* result = Result("这条日程已经存在。");
        cJSON_AddBoolToObject(result, "alreadyExists", true);
        cJSON_AddStringToObject(result, "eventId", existing->id.c_str());
        cJSON_AddStringToObject(result, "startsAt", existing->starts_at.c_str());
        AddNullableString(result, "endsAt", existing->ends_at);
        cJSON_AddStringToObject(result, "kind", existing->kind.c_str());
        return result;
    }

    std::vector<const Event*> conflicts;
    if (HasConflictLocked(event, &conflicts)) {
        const char* supplied_token = StringArg(arguments, "conflictConfirmationToken");
        const std::string token = ConflictToken(event, conflicts);
        if (supplied_token == nullptr || supplied_token != token) {
            cJSON* result = Error("时间与已有日程冲突，是否仍要创建？");
            cJSON_AddStringToObject(result, "reason", "calendar_conflict");
            cJSON_AddBoolToObject(result, "requiresConfirmation", true);
            cJSON_AddStringToObject(result, "confirmationToken", token.c_str());
            cJSON_AddStringToObject(result, "conflictConfirmationToken", token.c_str());
            cJSON_AddStringToObject(result, "requestedTitle", event.title.c_str());
            cJSON_AddStringToObject(result, "requestedStartAt", event.starts_at.c_str());
            cJSON* list = cJSON_AddArrayToObject(result, "conflicts");
            for (const Event* conflict : conflicts)
                cJSON_AddItemToArray(list, EventToPublicJson(*conflict));
            return result;
        }
    }

    Reminder reminder;
    reminder.id = NewId("reminder");
    reminder.event_id = event.id;
    reminder.original_start_at = event.starts_at;
    reminder.trigger_at = FormatUtc(remind_epoch);
    reminder.status = ReminderStatus::Scheduled;
    state_.events.push_back(event);
    state_.reminders.push_back(reminder);
    if (event.weak_reminder_enabled &&
        start_epoch - event.weak_reminder_minutes * 60LL > Now()) {
        Reminder weak;
        weak.id = NewId("reminder");
        weak.event_id = event.id;
        weak.original_start_at = event.starts_at;
        weak.trigger_at = FormatUtc(start_epoch - event.weak_reminder_minutes * 60LL);
        weak.weak = true;
        weak.status = ReminderStatus::Scheduled;
        state_.reminders.push_back(weak);
    }
    const std::string speech = "已创建" + event.title + "，" +
        FormatSpokenTimeRange(event.starts_at, event.ends_at, Now()) + "。";
    cJSON* result = Result(speech);
    cJSON_AddStringToObject(result, "eventId", event.id.c_str());
    cJSON_AddStringToObject(result, "startsAt", event.starts_at.c_str());
    AddNullableString(result, "endsAt", event.ends_at);
    cJSON_AddStringToObject(result, "kind", event.kind.c_str());
    cJSON_AddBoolToObject(result, "conflictConfirmed", !conflicts.empty());
    cJSON_AddBoolToObject(result, "weakReminderEnabled", event.weak_reminder_enabled);
    AddReceiptLocked("calendar_created", speech, result);
    AddUndoLocked("calendar_create", &event, &reminder, result);
    return CommitLocked(result, "calendar_create");
}

cJSON* VoiceLifeService::CalendarQuery(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_)
        return Error("日程服务尚未初始化");
    const char* range_start = StringArg(arguments, "rangeStart");
    const char* range_end = StringArg(arguments, "rangeEnd");
    if (range_start == nullptr || range_end == nullptr)
        return Error("需要 rangeStart 和 rangeEnd");
    const int64_t start = ParseIso8601(range_start);
    const int64_t end = ParseIso8601(range_end);
    if (start < 0 || end <= start)
        return Error("查询时间范围无效");
    PruneLocked(Now());
    struct Match {
        const Event* event = nullptr;
        std::string original_start_at;
    };
    std::vector<Match> matches;
    for (const auto& event : state_.events) {
        if (event.terminated || event.paused)
            continue;
        const int64_t event_start = ParseIso8601(event.starts_at);
        if (event_start < 0)
            continue;
        std::string occurrence_start = FormatUtc(event_start);
        for (int attempt = 0; attempt < 5000 && !occurrence_start.empty(); ++attempt) {
            const int64_t occurrence_epoch = ParseIso8601(occurrence_start);
            if (occurrence_epoch < 0 || occurrence_epoch >= end)
                break;
            const int64_t event_end =
                event.ends_at.empty() ? event_start + 1 : ParseIso8601(event.ends_at);
            const int64_t duration = event_end > event_start ? event_end - event_start : 1;
            const int64_t occurrence_end = occurrence_epoch + duration;
            if (!IsOccurrenceSkippedLocked(event, occurrence_start) &&
                ((occurrence_epoch >= start && occurrence_epoch < end) ||
                 (!event.ends_at.empty() && occurrence_end > start && occurrence_epoch < end))) {
                matches.push_back({&event, occurrence_start});
            }
            if (event.recurrence_frequency.empty())
                break;
            occurrence_start = NextOccurrenceUtc(event, occurrence_start);
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        return ParseIso8601(a.original_start_at) < ParseIso8601(b.original_start_at);
    });
    const bool truncated = matches.size() > kCalendarQueryReplyLimit;
    std::string speech;
    if (matches.empty()) {
        speech = "这个时间范围内没有安排。";
    } else {
        speech = "共有" + std::to_string(matches.size()) + "条安排：";
        const size_t spoken_count = std::min(matches.size(), kCalendarQueryReplyLimit);
        for (size_t i = 0; i < spoken_count; ++i) {
            if (i != 0)
                speech += "；";
            const Match& match = matches[i];
            const int64_t occurrence_start = ParseIso8601(match.original_start_at);
            const int64_t event_start = ParseIso8601(match.event->starts_at);
            std::string occurrence_end;
            if (!match.event->ends_at.empty() && occurrence_start >= 0 && event_start >= 0) {
                const int64_t event_end = ParseIso8601(match.event->ends_at);
                if (event_end > event_start) {
                    occurrence_end = FormatUtc(occurrence_start + event_end - event_start);
                }
            }
            speech += FormatSpokenTimeRange(match.original_start_at, occurrence_end, Now()) + "，" +
                      match.event->title;
        }
        speech += "。";
        if (truncated)
            speech += kCalendarQueryImTail;
    }
    cJSON* result = Result(speech);
    cJSON_AddNumberToObject(result, "total", static_cast<double>(matches.size()));
    cJSON* occurrences = cJSON_AddArrayToObject(result, "occurrences");
    const size_t returned_count = std::min(matches.size(), kCalendarQueryReplyLimit);
    for (size_t i = 0; i < returned_count; ++i) {
        cJSON_AddItemToArray(
            occurrences, OccurrenceToQueryJson(*matches[i].event, matches[i].original_start_at));
    }
    cJSON_AddNumberToObject(result, "returned", static_cast<double>(returned_count));
    cJSON_AddBoolToObject(result, "truncated", truncated);
    AddReceiptLocked("calendar_queried", speech, result);
    SaveLocked();
    return result;
}

cJSON* VoiceLifeService::CalendarFind(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_)
        return Error("日程服务尚未初始化");
    const char* query = StringArg(arguments, "query");
    if (query == nullptr || *query == '\0')
        return Error("需要 query");
    const char* range_start = StringArg(arguments, "rangeStart");
    const char* range_end = StringArg(arguments, "rangeEnd");
    if ((range_start == nullptr) != (range_end == nullptr))
        return Error("rangeStart 和 rangeEnd 必须一起填写");
    int64_t range_start_epoch = 0;
    int64_t range_end_epoch = 0;
    const bool has_range = range_start != nullptr;
    if (has_range) {
        range_start_epoch = ParseIso8601(range_start);
        range_end_epoch = ParseIso8601(range_end);
        if (range_start_epoch < 0 || range_end_epoch <= range_start_epoch)
            return Error("查询时间范围无效");
    }
    cJSON* result = Result("");
    cJSON* candidates = cJSON_AddArrayToObject(result, "candidates");
    struct Candidate {
        const Event* event = nullptr;
        std::string original_start_at;
    };
    std::vector<Candidate> matches;
    const auto collect_matches = [this, &matches, has_range, range_start_epoch, range_end_epoch,
                                  query](bool require_title_match) {
        matches.clear();
        for (const auto& event : state_.events) {
            if (event.terminated || (require_title_match && !Contains(event.title, query))) continue;
            if (!has_range) {
                matches.push_back({&event, event.starts_at});
                continue;
            }
            std::vector<std::string> occurrences;
            EnumerateOccurrencesLocked(event, range_start_epoch, range_end_epoch, &occurrences);
            for (const auto& occurrence : occurrences) matches.push_back({&event, occurrence});
        }
    };
    collect_matches(true);
    bool query_relaxed = false;
    if (matches.empty() && has_range) {
        collect_matches(false);
        query_relaxed = !matches.empty();
        if (matches.size() > 1) {
            size_t best_distance = std::string::npos;
            size_t second_distance = std::string::npos;
            size_t best_index = 0;
            for (size_t index = 0; index < matches.size(); ++index) {
                const size_t distance = EditDistance(query, matches[index].event->title);
                if (distance < best_distance) {
                    second_distance = best_distance;
                    best_distance = distance;
                    best_index = index;
                } else if (distance < second_distance) {
                    second_distance = distance;
                }
            }
            const size_t comparable_length =
                std::max(std::strlen(query), matches[best_index].event->title.size());
            if (best_distance < second_distance && best_distance * 2 < comparable_length) {
                const Candidate best = matches[best_index];
                matches.assign(1, best);
            }
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Candidate& left, const Candidate& right) {
        return ParseIso8601(left.original_start_at) < ParseIso8601(right.original_start_at);
    });
    const size_t returned = std::min(matches.size(), kCalendarFindReplyLimit);
    for (size_t index = 0; index < returned; ++index) {
        const Candidate& match = matches[index];
        cJSON_AddItemToArray(candidates,
                             CalendarFindCandidateJson(*match.event, match.original_start_at));
    }
    const int count = static_cast<int>(matches.size());
    std::string speech;
    if (count == 0) {
        speech = "没有找到符合条件的日程。";
    } else if (count == 1) {
        speech = "找到一条日程。";
    } else if (matches.size() > returned) {
        speech = "找到" + std::to_string(count) + "条日程，已返回前3条，请说更具体的标题或时间。";
    } else {
        speech = "找到" + std::to_string(count) + "条日程，请指定要修改哪一条。";
    }
    cJSON_ReplaceItemInObject(result, "speech", cJSON_CreateString(speech.c_str()));
    cJSON_AddNumberToObject(result, "total", count);
    cJSON_AddNumberToObject(result, "returned", static_cast<double>(returned));
    cJSON_AddBoolToObject(result, "truncated", matches.size() > returned);
    cJSON_AddBoolToObject(result, "requiresDisambiguation", count > 1);
    cJSON_AddBoolToObject(result, "queryRelaxed", query_relaxed);
    AddReceiptLocked("calendar_found", speech, result);
    SaveLocked();
    return result;
}

cJSON* VoiceLifeService::CalendarModify(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* event_id = StringArg(arguments, "eventId");
    if (!initialized_ || event_id == nullptr) return Error("需要 eventId");
    Event* event = FindEventLocked(event_id);
    if (event == nullptr) return Error("没有找到这条日程");
    if (const char* scope = StringArg(arguments, "scope")) {
        const std::string requested_scope = scope;
        if (event->recurrence_frequency.empty()) {
            if (requested_scope != "this_occurrence" && requested_scope != "entire_series") {
                return Error("单次日程只能修改这一次");
            }
        } else if (requested_scope != "entire_series") {
            return Error("当前 MVP 只支持修改整个周期系列");
        }
    }
    const std::string old_event = EventJson(*event);
    const Event before_event = *event;
    std::vector<Reminder> old_reminders;
    for (const auto& reminder : state_.reminders) {
        if (reminder.event_id == event->id) old_reminders.push_back(reminder);
    }
    const int64_t old_start_epoch = ParseIso8601(event->starts_at);
    const int64_t old_end_epoch = event->ends_at.empty() ? -1 : ParseIso8601(event->ends_at);
    Event candidate = *event;
    if (const char* value = StringArg(arguments, "title")) candidate.title = value;
    bool start_changed = false;
    if (const char* value = StringArg(arguments, "newStartAt")) { candidate.starts_at = value; start_changed = true; }
    if (const char* value = StringArg(arguments, "startsAt")) { candidate.starts_at = value; start_changed = true; }
    const bool explicit_end = cJSON_HasObjectItem(arguments, "endsAt");
    if (explicit_end) {
        const char* value = StringArg(arguments, "endsAt");
        if (value == nullptr) return Error("endsAt 必须是 ISO 8601 时间");
        candidate.ends_at = value;
    }
    if (const char* value = StringArg(arguments, "location")) candidate.location = value;
    if (const char* value = StringArg(arguments, "notes")) candidate.notes = value;
    const bool weak_changed = cJSON_HasObjectItem(arguments, "weakReminder");
    if (weak_changed) candidate.weak_reminder_enabled = BoolArg(arguments, "weakReminder");
    const int64_t candidate_start_epoch = ParseIso8601(candidate.starts_at);
    if (candidate.starts_at.empty() || candidate_start_epoch < 0) return Error("新的开始时间格式无效");
    if (candidate_start_epoch < Now() - 1) return Error("新的日程时间已经过去");
    if (start_changed && !explicit_end && old_start_epoch >= 0 && old_end_epoch > old_start_epoch) {
        candidate.ends_at = FormatUtc(candidate_start_epoch + old_end_epoch - old_start_epoch);
    }
    if (candidate.kind == "time_block" && candidate.ends_at.empty()) return Error("时间段日程需要结束时间");
    if (candidate.kind == "point" && !candidate.ends_at.empty()) return Error("时间点日程不能设置结束时间");
    if (!candidate.ends_at.empty() && ParseIso8601(candidate.ends_at) <= ParseIso8601(candidate.starts_at)) return Error("结束时间必须晚于开始时间");
    if (start_changed && candidate.recurrence_frequency == "weekly") {
        candidate.recurrence_weekday = Iso8601LocalWeekday(candidate.starts_at);
    }
    if (start_changed && candidate.recurrence_frequency == "monthly") {
        candidate.recurrence_month_day = Iso8601LocalMonthDay(candidate.starts_at);
    }
    std::vector<const Event*> conflicts;
    if (HasConflictLocked(candidate, &conflicts)) {
        const std::string token = ConflictToken(candidate, conflicts);
        const char* supplied = StringArg(arguments, "conflictConfirmationToken");
        if (supplied == nullptr || supplied != token) {
            cJSON* result = Error("修改后的时间与已有日程冲突，是否仍要修改？");
            cJSON_AddBoolToObject(result, "requiresConfirmation", true);
            cJSON_AddStringToObject(result, "confirmationToken", token.c_str());
            cJSON* list = cJSON_AddArrayToObject(result, "conflicts");
            for (const Event* conflict : conflicts)
                cJSON_AddItemToArray(list, EventToPublicJson(*conflict));
            return result;
        }
    }
    candidate.updated_at = Now();
    *event = candidate;
    const bool schedule_changed = start_changed || explicit_end;
    if (schedule_changed) {
        state_.reminders.erase(std::remove_if(state_.reminders.begin(), state_.reminders.end(),
                                              [event_id](const Reminder& reminder) { return reminder.event_id == event_id; }),
                               state_.reminders.end());
        EnsureRemindersForOccurrenceLocked(candidate, candidate.starts_at, false);
    } else if (weak_changed) {
        if (!candidate.weak_reminder_enabled) {
            state_.reminders.erase(std::remove_if(state_.reminders.begin(), state_.reminders.end(),
                                                  [event_id](const Reminder& reminder) {
                                                      return reminder.event_id == event_id && reminder.weak;
                                                  }),
                                   state_.reminders.end());
        } else {
            EnsureRemindersForOccurrenceLocked(candidate, candidate.starts_at, true);
        }
    }
    const std::string speech = "已修改" + candidate.title + "，" +
        FormatSpokenTimeRange(candidate.starts_at, candidate.ends_at, Now()) + "。";
    cJSON* result = Result(speech);
    cJSON_AddItemToObject(result, "event", EventToPublicJson(candidate));
    AddReceiptLocked("calendar_modified", speech, result);
    AddUndoLocked("calendar_modify", &candidate, nullptr, result, &old_reminders);
    if (!state_.undo_operations.empty()) {
        state_.undo_operations.back().event_snapshot = old_event;
        state_.undo_operations.back().event_snapshot = EventJson(before_event);
        state_.undo_operations.back().reminder_snapshot = old_reminders.empty() ? std::string{} : ReminderJson(old_reminders.front());
        state_.undo_operations.back().reminder_snapshots.clear();
        for (const auto& reminder : old_reminders) {
            state_.undo_operations.back().reminder_snapshots.push_back(ReminderJson(reminder));
        }
    }
    return CommitLocked(result, "calendar_modify");
}

cJSON* VoiceLifeService::CalendarSkipOccurrence(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* event_id = StringArg(arguments, "eventId");
    if (!initialized_ || event_id == nullptr) return Error("需要 eventId");
    Event* event = FindEventLocked(event_id);
    if (event == nullptr) return Error("没有找到这条日程");
    const char* original = StringArg(arguments, "originalStartAt");
    const int64_t requested_epoch = original == nullptr ? ParseIso8601(event->starts_at) : ParseIso8601(original);
    const std::string occurrence_start = requested_epoch < 0 ? std::string{} : FormatUtc(requested_epoch);
    if (occurrence_start.empty() || !IsOccurrenceInSeriesLocked(*event, occurrence_start)) return Error("指定时间不是该日程的有效周期实例");
    if (event->recurrence_frequency.empty() && event->terminated) {
        cJSON* result = Result("这条日程已经取消。");
        cJSON_AddBoolToObject(result, "alreadySkipped", true);
        cJSON_AddStringToObject(result, "eventId", event->id.c_str());
        return result;
    }
    if (!event->recurrence_frequency.empty()) {
        if (IsOccurrenceSkippedLocked(*event, occurrence_start)) {
            cJSON* result = Result("这次日程已经跳过。");
            cJSON_AddBoolToObject(result, "alreadySkipped", true);
            cJSON_AddStringToObject(result, "eventId", event->id.c_str());
            return result;
        }
        const Event before = *event;
        event->skipped_occurrences.push_back(occurrence_start);
        event->updated_at = Now();
        const std::string speech = "已跳过这一次日程，下一次仍会提醒。";
        cJSON* result = Result(speech);
        cJSON_AddBoolToObject(result, "alreadySkipped", false);
        cJSON_AddStringToObject(result, "eventId", event->id.c_str());
        AddReceiptLocked("calendar_skipped", speech, result);
        AddUndoLocked("calendar_skip", &before, nullptr, result);
        EnsureNextReminderLocked(*event, occurrence_start);
        return CommitLocked(result, "calendar_skip_occurrence");
    }
    const Event before = *event;
    event->terminated = true;
    event->updated_at = Now();
    const std::string speech = "已取消" + event->title + "。";
    cJSON* result = Result(speech);
    AddReceiptLocked("calendar_skipped", speech, result);
    AddUndoLocked("calendar_skip", &before, nullptr, result);
    return CommitLocked(result, "calendar_skip_occurrence");
}

cJSON* VoiceLifeService::CalendarPauseSeries(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* event_id = StringArg(arguments, "eventId");
    if (!initialized_ || event_id == nullptr) return Error("需要 eventId");
    Event* event = FindEventLocked(event_id);
    if (event == nullptr) return Error("没有找到这条日程");
    if (event->recurrence_frequency.empty()) return Error("只有周期日程可以暂停");
    if (event->paused) {
        cJSON* result = Result("这组日程已经暂停。");
        cJSON_AddBoolToObject(result, "alreadyPaused", true);
        return result;
    }
    const Event before = *event;
    event->paused = true;
    event->updated_at = Now();
    const std::string speech = "已暂停这组日程。";
    cJSON* result = Result(speech);
    AddReceiptLocked("calendar_paused", speech, result);
    AddUndoLocked("calendar_pause", &before, nullptr, result);
    return CommitLocked(result, "calendar_pause_series");
}

cJSON* VoiceLifeService::CalendarResumeSeries(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* event_id = StringArg(arguments, "eventId");
    if (!initialized_ || event_id == nullptr) return Error("需要 eventId");
    Event* event = FindEventLocked(event_id);
    if (event == nullptr) return Error("没有找到这条日程");
    if (event->recurrence_frequency.empty()) return Error("只有周期日程可以恢复");
    if (!event->paused) {
        cJSON* result = Result("这组日程已经在运行。");
        cJSON_AddBoolToObject(result, "alreadyResumed", true);
        return result;
    }
    const Event before = *event;
    event->paused = false;
    event->updated_at = Now();
    std::vector<Reminder> old_reminders;
    for (const auto& reminder : state_.reminders) {
        if (reminder.event_id == event->id) old_reminders.push_back(reminder);
    }
    // Do not replay occurrences that became due while the series was paused.
    // Close those stale reminders and materialize the next future occurrence.
    const int64_t now = Now();
    for (auto& reminder : state_.reminders) {
        if (reminder.event_id == event->id && reminder.status != ReminderStatus::Closed &&
            ParseIso8601(reminder.trigger_at) <= now) {
            reminder.status = ReminderStatus::Closed;
            reminder.closed_at = now;
        }
    }
    std::string cursor = FormatUtc(ParseIso8601(event->starts_at));
    for (int attempt = 0; attempt < 5000 && !cursor.empty(); ++attempt) {
        if (ParseIso8601(cursor) > now && !IsOccurrenceSkippedLocked(*event, cursor)) {
            EnsureRemindersForOccurrenceLocked(*event, cursor, true);
            break;
        }
        if (event->recurrence_frequency.empty()) break;
        cursor = NextOccurrenceUtc(*event, cursor);
    }
    const std::string speech = "已恢复这组日程。";
    cJSON* result = Result(speech);
    AddReceiptLocked("calendar_resumed", speech, result);
    AddUndoLocked("calendar_resume", &before, nullptr, result, &old_reminders);
    return CommitLocked(result, "calendar_resume_series");
}

cJSON* VoiceLifeService::CalendarTerminateSeries(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* event_id = StringArg(arguments, "eventId");
    if (!initialized_ || event_id == nullptr) return Error("需要 eventId");
    Event* event = FindEventLocked(event_id);
    if (event == nullptr) return Error("没有找到这条日程");
    if (event->recurrence_frequency.empty()) return Error("只有周期日程可以终止");
    if (event->terminated) {
        cJSON* result = Result("这组日程已经终止。");
        cJSON_AddBoolToObject(result, "alreadyTerminated", true);
        return result;
    }
    const std::string token = "terminate-" + event->id;
    const char* supplied = StringArg(arguments, "confirmationToken");
    if (supplied == nullptr || supplied != token) {
        cJSON* result = Error("终止日程会影响后续周期，是否确认？");
        cJSON_AddBoolToObject(result, "requiresConfirmation", true);
        cJSON_AddStringToObject(result, "confirmationToken", token.c_str());
        return result;
    }
    const Event before = *event;
    event->terminated = true;
    event->updated_at = Now();
    std::vector<Reminder> old_reminders;
    for (auto& reminder : state_.reminders) {
        if (reminder.event_id != event->id) continue;
        old_reminders.push_back(reminder);
        if (reminder.status != ReminderStatus::Closed) {
            reminder.status = ReminderStatus::Closed;
            reminder.closed_at = Now();
        }
    }
    const std::string speech = "已终止后续日程。";
    cJSON* result = Result(speech);
    AddReceiptLocked("calendar_terminated", speech, result);
    AddUndoLocked("calendar_terminate", &before, nullptr, result, &old_reminders);
    return CommitLocked(result, "calendar_terminate_series");
}

cJSON* VoiceLifeService::CalendarDelete(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* event_id = StringArg(arguments, "eventId");
    if (!initialized_ || event_id == nullptr)
        return Error("需要 eventId");
    Event* event = FindEventLocked(event_id);
    if (event == nullptr)
        return Error("没有找到这条日程");
    const std::string token = "delete-" + event->id;
    const char* supplied = StringArg(arguments, "confirmationToken");
    if (supplied == nullptr) supplied = StringArg(arguments, "conflictConfirmationToken");
    if (supplied == nullptr || supplied != token) {
        cJSON* result = Error("是否确认删除这条日程？");
        cJSON_AddBoolToObject(result, "requiresConfirmation", true);
        cJSON_AddStringToObject(result, "confirmationToken", token.c_str());
        return result;
    }
    Event snapshot = *event;
    std::vector<Reminder> reminders;
    for (const auto& item : state_.reminders) {
        if (item.event_id == event->id) reminders.push_back(item);
    }
    state_.events.erase(std::remove_if(state_.events.begin(), state_.events.end(),
                                      [event_id](const Event& item) { return item.id == event_id; }), state_.events.end());
    state_.reminders.erase(std::remove_if(state_.reminders.begin(), state_.reminders.end(),
                                         [event_id](const Reminder& item) { return item.event_id == event_id; }), state_.reminders.end());
    const std::string speech = "已删除" + snapshot.title + "。";
    cJSON* result = Result(speech);
    AddReceiptLocked("calendar_deleted", speech, result);
    AddUndoLocked("calendar_delete", &snapshot, nullptr, result, &reminders);
    return CommitLocked(result, "calendar_delete");
}

cJSON* VoiceLifeService::CalendarUndo(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* operation_id = StringArg(arguments, "undoOperationId");
    if (!initialized_ || operation_id == nullptr) return Error("需要 undoOperationId");
    auto it = std::find_if(state_.undo_operations.begin(), state_.undo_operations.end(),
                           [operation_id](const UndoOperation& operation) { return operation.id == operation_id; });
    if (it == state_.undo_operations.end() || it->undone || it->expires_at <= Now()) return Error("撤销操作不存在或已过期");
    const UndoOperation operation = *it;
    if (operation.action == "calendar_create") {
        Event event = EventFromSnapshot(operation.event_snapshot);
        state_.events.erase(std::remove_if(state_.events.begin(), state_.events.end(),
                                           [&event](const Event& item) { return item.id == event.id; }), state_.events.end());
        state_.reminders.erase(std::remove_if(state_.reminders.begin(), state_.reminders.end(),
                                              [&event](const Reminder& item) { return item.event_id == event.id; }), state_.reminders.end());
    } else {
        Event event = EventFromSnapshot(operation.event_snapshot);
        if (event.id.empty()) return Error("撤销快照无效");
        Event* current = FindEventLocked(event.id);
        if (current == nullptr) state_.events.push_back(event);
        else *current = event;

        std::vector<std::string> snapshots = operation.reminder_snapshots;
        if (snapshots.empty() && !operation.reminder_snapshot.empty()) {
            snapshots.push_back(operation.reminder_snapshot);
        }
        const bool restore_reminders = operation.action == "calendar_modify" ||
                                       operation.action == "calendar_delete" ||
                                       operation.action == "calendar_resume" ||
                                       operation.action == "calendar_terminate";
        if (restore_reminders) {
            state_.reminders.erase(std::remove_if(state_.reminders.begin(), state_.reminders.end(),
                                                  [&event](const Reminder& item) { return item.event_id == event.id; }),
                                   state_.reminders.end());
            for (const auto& snapshot : snapshots) {
                Reminder reminder = ReminderFromSnapshot(snapshot);
                if (!reminder.id.empty()) state_.reminders.push_back(std::move(reminder));
            }
        }
        Event* restored = FindEventLocked(event.id);
        if (restored != nullptr && snapshots.empty() && operation.action == "calendar_skip") {
            EnsureRemindersForOccurrenceLocked(*restored, restored->starts_at, true);
        }
    }
    it->undone = true;
    const std::string speech = "已撤销上一次操作。";
    cJSON* result = Result(speech);
    AddReceiptLocked("calendar_undo", speech, result);
    return CommitLocked(result, "calendar_undo");
}

cJSON* VoiceLifeService::ReminderListDue(const cJSON* /*arguments*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return Error("日程服务尚未初始化");
    const int64_t now = Now();
    PruneLocked(now);
    std::vector<const Reminder*> due;
    for (const auto& reminder : state_.reminders) {
        if (reminder.status == ReminderStatus::Closed) continue;
        const Event* event = FindEventLocked(reminder.event_id);
        if (event == nullptr || event->paused || event->terminated) continue;
        if (IsOccurrenceSkippedLocked(*event, reminder.original_start_at)) continue;
        if (ParseIso8601(reminder.trigger_at) <= now) due.push_back(&reminder);
    }
    std::string speech;
    if (due.empty()) {
        speech = "现在没有到期提醒。";
    } else if (due.size() == 1) {
        const Event* event = FindEventLocked(due[0]->event_id);
        speech = "有一条到期提醒：" +
            (event == nullptr ? std::string("未命名事项") : event->title) + "。";
    } else {
        speech = "现在有" + std::to_string(due.size()) + "条到期提醒，请指定要处理哪一条。";
    }
    cJSON* result = Result(speech);
    cJSON* reminders = cJSON_AddArrayToObject(result, "reminders");
    for (const Reminder* reminder : due) cJSON_AddItemToArray(reminders, ReminderToPublicJson(*reminder, FindEventLocked(reminder->event_id)));
    cJSON_AddBoolToObject(result, "requiresDisambiguation", due.size() > 1);
    cJSON_AddNumberToObject(result, "total", static_cast<double>(due.size()));
    AddReceiptLocked("reminder_list_due", speech, result);
    SaveLocked();
    return result;
}

cJSON* VoiceLifeService::ReminderClose(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* reminder_id = StringArg(arguments, "reminderId");
    if (!initialized_ || reminder_id == nullptr) return Error("需要 reminderId");
    Reminder* reminder = FindReminderLocked(reminder_id);
    if (reminder == nullptr) return Error("没有找到这条提醒");
    const bool already_closed = reminder->status == ReminderStatus::Closed;
    reminder->status = ReminderStatus::Closed;
    reminder->closed_at = Now();
    const std::string speech = already_closed ? "这条提醒已经关闭。" : "好的，已关闭提醒，日程保持不变。";
    cJSON* result = Result(speech);
    cJSON_AddBoolToObject(result, "alreadyClosed", already_closed);
    cJSON_AddStringToObject(result, "reminderId", reminder->id.c_str());
    AddReceiptLocked("reminder_closed", speech, result);
    return CommitLocked(result, "reminder_close");
}

cJSON* VoiceLifeService::ReminderSnooze(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* reminder_id = StringArg(arguments, "reminderId");
    const cJSON* minutes_value = cJSON_GetObjectItemCaseSensitive(arguments, "minutes");
    const int minutes = cJSON_IsNumber(minutes_value) ? minutes_value->valueint : 0;
    if (minutes_value == nullptr || !cJSON_IsNumber(minutes_value) ||
        std::floor(minutes_value->valuedouble) != minutes_value->valuedouble) {
        return Error("需要有效的 reminderId 和 1 到 1440 分钟");
    }
    if (!initialized_ || reminder_id == nullptr || minutes < 1 || minutes > 1440) return Error("需要有效的 reminderId 和 1 到 1440 分钟");
    Reminder* reminder = FindReminderLocked(reminder_id);
    if (reminder == nullptr) return Error("没有找到这条提醒");
    if (reminder->status == ReminderStatus::Closed) return Error("这条提醒已经关闭");
    if (reminder->status == ReminderStatus::Snoozed) {
        cJSON* result = Result("这条提醒已经推迟过了。");
        cJSON_AddBoolToObject(result, "alreadySnoozed", true);
        cJSON_AddNumberToObject(result, "snoozeCount", reminder->snooze_count);
        AddString(result, "nextTriggerAt", reminder->trigger_at);
        return result;
    }
    if (reminder->status == ReminderStatus::Scheduled && ParseIso8601(reminder->trigger_at) > Now()) {
        return Error("这条提醒还没有到期");
    }
    if (reminder->status != ReminderStatus::Pushed && reminder->status != ReminderStatus::Scheduled) {
        return Error("这条提醒当前不能推迟");
    }
    if (reminder->snooze_count >= 3) return Error("这条提醒已经推迟三次，不能继续推迟");
    reminder->snooze_count++;
    reminder->status = ReminderStatus::Snoozed;
    reminder->trigger_at = FormatUtc(Now() + minutes * 60LL);
    reminder->im_reported_trigger_at.clear();
    const std::string speech = "好的，" + std::to_string(minutes) + "分钟后再次提醒，日程保持不变。";
    cJSON* result = Result(speech);
    cJSON_AddBoolToObject(result, "alreadySnoozed", false);
    cJSON_AddNumberToObject(result, "snoozeCount", reminder->snooze_count);
    AddString(result, "nextTriggerAt", reminder->trigger_at);
    AddReceiptLocked("reminder_snoozed", speech, result);
    return CommitLocked(result, "reminder_snooze");
}

cJSON* VoiceLifeService::ReminderGetDetails(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* reminder_id = StringArg(arguments, "reminderId");
    if (!initialized_ || reminder_id == nullptr) return Error("需要 reminderId");
    const Reminder* reminder = FindReminderLocked(reminder_id);
    if (reminder == nullptr) return Error("没有找到这条提醒");
    const Event* event = FindEventLocked(reminder->event_id);
    if (event == nullptr) return Error("提醒关联的日程不存在");
    Event occurrence = *event;
    const int64_t original_start = ParseIso8601(reminder->original_start_at);
    const int64_t event_start = ParseIso8601(event->starts_at);
    if (original_start >= 0 && event_start >= 0) {
        occurrence.starts_at = FormatUtc(original_start);
        if (!event->ends_at.empty()) {
            const int64_t event_end = ParseIso8601(event->ends_at);
            if (event_end > event_start) occurrence.ends_at = FormatUtc(original_start + event_end - event_start);
        }
    }
    const std::string speech = event->title + "，" +
        FormatSpokenTimeRange(occurrence.starts_at, occurrence.ends_at, Now()) +
        (occurrence.location.empty() ? std::string{} : "，地点" + occurrence.location) +
        (occurrence.notes.empty() ? std::string{} : "，备注" + occurrence.notes) + "。";
    cJSON* result = Result(speech);
    cJSON_AddItemToObject(result, "reminder", ReminderToPublicJson(*reminder, event));
    AddReceiptLocked("reminder_details", speech, result);
    SaveLocked();
    return result;
}

cJSON* VoiceLifeService::NoteRecord(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* content = StringArg(arguments, "content");
    if (!initialized_ || content == nullptr || *content == '\0') return Error("需要记录内容");
    if (IsSensitiveNote(content)) return Error("这条内容看起来包含密码、验证码或令牌，不能作为临时记录保存");
    ShortNote note;
    note.id = NewId("note");
    note.content = content;
    if (const char* category = StringArg(arguments, "category")) note.category = category;
    note.created_at = Now();
    note.expires_at = note.created_at + kNoteLifetimeSeconds;
    state_.notes.push_back(note);
    while (state_.notes.size() > 64) state_.notes.erase(state_.notes.begin());
    const std::string speech = "记住了：" + note.content + "。这条临时记录保留二十四小时。";
    cJSON* result = Result(speech);
    cJSON* public_note = cJSON_AddObjectToObject(result, "note");
    AddString(public_note, "noteId", note.id);
    AddString(public_note, "content", note.content);
    AddNullableString(public_note, "category", note.category);
    cJSON_AddNumberToObject(public_note, "expiresAt", static_cast<double>(note.expires_at));
    AddReceiptLocked("note_recorded", speech, result);
    return CommitLocked(result, "note_record");
}

cJSON* VoiceLifeService::NoteQuery(const cJSON* arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return Error("日程服务尚未初始化");
    PruneLocked(Now());
    const char* query = StringArg(arguments, "query");
    cJSON* result = Result("");
    cJSON* notes = cJSON_AddArrayToObject(result, "notes");
    int count = 0;
    for (const auto& note : state_.notes) {
        if (query != nullptr && !Contains(note.content, query)) continue;
        cJSON* item = cJSON_CreateObject();
        AddString(item, "noteId", note.id);
        AddString(item, "content", note.content);
        AddNullableString(item, "category", note.category);
        cJSON_AddNumberToObject(item, "expiresAt", static_cast<double>(note.expires_at));
        cJSON_AddItemToArray(notes, item);
        ++count;
    }
    const std::string speech = count == 0 ? "没有找到仍在有效期内的临时记录。" : "找到" + std::to_string(count) + "条临时记录。";
    cJSON_ReplaceItemInObject(result, "speech", cJSON_CreateString(speech.c_str()));
    cJSON_AddNumberToObject(result, "total", count);
    AddReceiptLocked("note_queried", speech, result);
    SaveLocked();
    return result;
}

std::vector<std::string> VoiceLifeService::CollectPendingImNotifications(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> payloads;
    if (!initialized_ || device_id.empty()) return payloads;
    const int64_t now = Now();
    for (const auto& reminder : state_.reminders) {
        if (reminder.status == ReminderStatus::Closed ||
            ParseIso8601(reminder.trigger_at) > now ||
            reminder.im_reported_trigger_at == reminder.trigger_at) {
            continue;
        }
        const Event* event = FindEventLocked(reminder.event_id);
        if (event == nullptr || event->paused || event->terminated ||
            IsOccurrenceSkippedLocked(*event, reminder.original_start_at)) {
            continue;
        }
        cJSON* payload = cJSON_CreateObject();
        AddString(payload, "deviceId", device_id);
        AddString(payload, "reminderId", reminder.id);
        AddString(payload, "occurrence", reminder.original_start_at);
        AddString(payload, "originalStartAt", reminder.original_start_at);
        AddString(payload, "title", event->title);
        AddString(payload, "dueAt", reminder.trigger_at);
        cJSON_AddBoolToObject(payload, "weak", reminder.weak);
        cJSON_AddNumberToObject(payload, "snoozeCount", reminder.snooze_count);
        AddString(payload, "deliveryKey", device_id + "|" + reminder.id + "|" + reminder.trigger_at);
        payloads.push_back(PrintJson(payload));
    }
    return payloads;
}

bool VoiceLifeService::MarkImNotificationReported(const std::string& reminder_id,
                                                   const std::string& trigger_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || reminder_id.empty() || trigger_at.empty()) return false;
    Reminder* reminder = FindReminderLocked(reminder_id);
    if (reminder == nullptr || reminder->trigger_at != trigger_at || reminder->status == ReminderStatus::Closed) {
        return false;
    }
    reminder->im_reported_trigger_at = trigger_at;
    return SaveLocked();
}

std::string VoiceLifeService::ApplyImActionLocked(const std::string& action_id,
                                                  const std::string& type,
                                                  const std::string& reminder_id,
                                                  int minutes) {
    auto existing = std::find_if(state_.processed_im_actions.begin(), state_.processed_im_actions.end(),
                                 [&action_id](const ProcessedImAction& action) {
                                     return action.action_id == action_id;
                                 });
    if (existing != state_.processed_im_actions.end()) return existing->result_json;
    if (action_id.empty()) return PrintJson(Error("需要 actionId"));

    const auto finalize = [this, &action_id](cJSON* result, bool ok) -> std::string {
        const std::string json = PrintJson(result);
        ProcessedImAction processed;
        processed.action_id = action_id;
        processed.ok = ok;
        processed.result_json = json;
        processed.processed_at = Now();
        state_.processed_im_actions.push_back(std::move(processed));
        while (state_.processed_im_actions.size() > 64) state_.processed_im_actions.erase(state_.processed_im_actions.begin());
        if (SaveLocked()) return json;
        return PrintJson(Error("本地保存失败，请重试"));
    };

    std::string effective_type = type;
    if (effective_type == "dismiss") effective_type = "close";
    if (effective_type != "close" && effective_type != "snooze") {
        return finalize(Error("未知的设备操作"), false);
    }
    Reminder* reminder = FindReminderLocked(reminder_id);
    if (reminder == nullptr) return finalize(Error("没有找到这条提醒"), false);
    if (effective_type == "close") {
        const bool already_closed = reminder->status == ReminderStatus::Closed;
        reminder->status = ReminderStatus::Closed;
        reminder->closed_at = Now();
        cJSON* result = Result(already_closed ? "这条提醒已经关闭。" : "好的，已关闭提醒，日程保持不变。");
        cJSON_AddBoolToObject(result, "alreadyClosed", already_closed);
        AddString(result, "reminderId", reminder->id);
        AddReceiptLocked("reminder_closed_by_im", cJSON_GetObjectItem(result, "speech")->valuestring, result);
        return finalize(result, true);
    }

    if (minutes < 1 || minutes > 1440) return finalize(Error("需要有效的 1 到 1440 分钟"), false);
    if (reminder->status == ReminderStatus::Closed) return finalize(Error("这条提醒已经关闭"), false);
    if (reminder->status == ReminderStatus::Snoozed) {
        cJSON* result = Result("这条提醒已经推迟过了。");
        cJSON_AddBoolToObject(result, "alreadySnoozed", true);
        cJSON_AddNumberToObject(result, "snoozeCount", reminder->snooze_count);
        AddString(result, "nextTriggerAt", reminder->trigger_at);
        return finalize(result, true);
    }
    if (reminder->status == ReminderStatus::Scheduled && ParseIso8601(reminder->trigger_at) > Now()) {
        return finalize(Error("这条提醒还没有到期"), false);
    }
    if (reminder->status != ReminderStatus::Pushed && reminder->status != ReminderStatus::Scheduled) {
        return finalize(Error("这条提醒当前不能推迟"), false);
    }
    if (reminder->snooze_count >= 3) return finalize(Error("这条提醒已经推迟三次，不能继续推迟"), false);
    ++reminder->snooze_count;
    reminder->status = ReminderStatus::Snoozed;
    reminder->trigger_at = FormatUtc(Now() + minutes * 60LL);
    reminder->im_reported_trigger_at.clear();
    const std::string speech = "好的，" + std::to_string(minutes) + "分钟后再次提醒，日程保持不变。";
    cJSON* result = Result(speech);
    cJSON_AddBoolToObject(result, "alreadySnoozed", false);
    cJSON_AddNumberToObject(result, "snoozeCount", reminder->snooze_count);
    AddString(result, "nextTriggerAt", reminder->trigger_at);
    AddReceiptLocked("reminder_snoozed_by_im", speech, result);
    return finalize(result, true);
}

std::string VoiceLifeService::ApplyImAction(const std::string& action_id,
                                            const std::string& type,
                                            const std::string& reminder_id,
                                            int minutes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return PrintJson(Error("日程服务尚未初始化"));
    return ApplyImActionLocked(action_id, type, reminder_id, minutes);
}

void VoiceLifeService::Tick() {
    SpeechCallback callback;
    std::string announcement;
    std::vector<std::string> pending_ids;
    std::vector<std::string> lines;
    std::vector<std::pair<std::string, std::string>> completed_occurrences;
    bool suppressed_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return;
        const int64_t now = Now();
        PruneLocked(now);
        for (const auto& reminder : state_.reminders) {
            if (reminder.status != ReminderStatus::Scheduled && reminder.status != ReminderStatus::Snoozed) continue;
            const Event* event = FindEventLocked(reminder.event_id);
            if (event == nullptr || event->paused || event->terminated) continue;
            if (IsOccurrenceSkippedLocked(*event, reminder.original_start_at)) continue;
            if (ParseIso8601(reminder.trigger_at) > now) continue;
            if (reminder.snooze_count >= 3) {
                // Keep the receipt but suppress repeated voice delivery after
                // the third snooze, matching the #62 behavior.
                auto* mutable_reminder = FindReminderLocked(reminder.id);
                if (mutable_reminder != nullptr) {
                    mutable_reminder->status = ReminderStatus::Pushed;
                    mutable_reminder->delivered_at = now;
                    suppressed_changed = true;
                    completed_occurrences.emplace_back(event->id, reminder.original_start_at);
                }
                continue;
            }
            pending_ids.push_back(reminder.id);
            std::string occurrence_start = reminder.original_start_at.empty()
                ? event->starts_at : reminder.original_start_at;
            if (reminder.weak) {
                lines.push_back("提前提示：" + FormatSpokenTime(occurrence_start, now) +
                                "有" + event->title + "。");
            } else {
                lines.push_back("提醒：" + event->title + "到时间了。");
            }
        }
        if (!lines.empty()) {
            announcement = "【系统到期播报】请只重复下一行内容，不要调用工具，不要添加解释。\n";
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i != 0) announcement += "、";
                announcement += lines[i];
            }
            callback = speech_callback_;
        } else if (!pending_ids.empty() || suppressed_changed) {
            // No cloud callback yet: leave these reminders scheduled for a
            // later connected session.
            SaveLocked();
        }
        for (const auto& completed : completed_occurrences) {
            const Event* event = FindEventLocked(completed.first);
            if (event != nullptr) EnsureNextReminderLocked(*event, completed.second);
        }
        if (!completed_occurrences.empty() && announcement.empty()) SaveLocked();
    }
    if (announcement.empty() || !callback) return;

    ESP_LOGI(kTag, "Delivering %u due reminder(s) through Linx",
             static_cast<unsigned>(pending_ids.size()));
    if (!callback(announcement)) {
        ESP_LOGW(kTag, "Reminder is queued but Linx audio channel is not ready");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now = Now();
        for (const auto& reminder_id : pending_ids) {
            auto* reminder = FindReminderLocked(reminder_id);
            if (reminder == nullptr || reminder->status == ReminderStatus::Closed) continue;
            reminder->status = ReminderStatus::Pushed;
            reminder->delivered_at = now;
            const Event* event = FindEventLocked(reminder->event_id);
            if (event != nullptr) completed_occurrences.emplace_back(event->id, reminder->original_start_at);
        }
        for (const auto& completed : completed_occurrences) {
            const Event* event = FindEventLocked(completed.first);
            if (event != nullptr) EnsureNextReminderLocked(*event, completed.second);
        }
        AddReceiptLocked("reminder_due", announcement, nullptr);
        SaveLocked();
        ESP_LOGI(kTag, "Marked %u reminder(s) as pushed",
                 static_cast<unsigned>(pending_ids.size()));
    }
}

}  // namespace voicelife
