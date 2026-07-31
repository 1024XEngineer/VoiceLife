#include "voicelife_storage.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_spiffs.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <unistd.h>

namespace voicelife {
namespace {

constexpr char kTag[] = "VoiceLifeStorage";
constexpr char kMountPoint[] = "/voicelife";
constexpr char kStatePath[] = "/voicelife/state.json";
constexpr char kTempPath[] = "/voicelife/state.json.tmp";
constexpr char kBackupPath[] = "/voicelife/state.json.bak";
constexpr char kPartitionLabel[] = "voicelife";

enum class ReadResult {
    Missing,
    Invalid,
    Loaded,
};

const char* StringValue(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : nullptr;
}

int64_t IntValue(const cJSON* object, const char* key, int64_t fallback = 0) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) ? static_cast<int64_t>(value->valuedouble) : fallback;
}

bool BoolValue(const cJSON* object, const char* key, bool fallback = false) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}

void AddString(cJSON* object, const char* key, const std::string& value) {
    cJSON_AddStringToObject(object, key, value.c_str());
}

cJSON* EventToJson(const Event& event) {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "id", event.id);
    AddString(object, "title", event.title);
    AddString(object, "startsAt", event.starts_at);
    AddString(object, "endsAt", event.ends_at);
    AddString(object, "kind", event.kind);
    AddString(object, "timeZone", event.time_zone);
    AddString(object, "location", event.location);
    AddString(object, "notes", event.notes);
    AddString(object, "recurrenceFrequency", event.recurrence_frequency);
    cJSON_AddNumberToObject(object, "recurrenceWeekday", event.recurrence_weekday);
    cJSON_AddNumberToObject(object, "recurrenceMonthDay", event.recurrence_month_day);
    cJSON_AddNumberToObject(object, "reminderOffsetMinutes", event.reminder_offset_minutes);
    cJSON_AddBoolToObject(object, "weakReminderEnabled", event.weak_reminder_enabled);
    cJSON_AddNumberToObject(object, "weakReminderMinutes", event.weak_reminder_minutes);
    cJSON_AddBoolToObject(object, "paused", event.paused);
    cJSON_AddBoolToObject(object, "terminated", event.terminated);
    cJSON* skipped = cJSON_AddArrayToObject(object, "skippedOccurrences");
    for (const auto& occurrence : event.skipped_occurrences) {
        cJSON_AddItemToArray(skipped, cJSON_CreateString(occurrence.c_str()));
    }
    cJSON_AddNumberToObject(object, "createdAt", static_cast<double>(event.created_at));
    cJSON_AddNumberToObject(object, "updatedAt", static_cast<double>(event.updated_at));
    return object;
}

Event EventFromJson(const cJSON* object) {
    Event event;
    if (const char* value = StringValue(object, "id")) event.id = value;
    if (const char* value = StringValue(object, "title")) event.title = value;
    if (const char* value = StringValue(object, "startsAt")) event.starts_at = value;
    if (const char* value = StringValue(object, "endsAt")) event.ends_at = value;
    if (const char* value = StringValue(object, "kind")) event.kind = value;
    if (const char* value = StringValue(object, "timeZone")) event.time_zone = value;
    if (const char* value = StringValue(object, "location")) event.location = value;
    if (const char* value = StringValue(object, "notes")) event.notes = value;
    if (const char* value = StringValue(object, "recurrenceFrequency")) event.recurrence_frequency = value;
    event.recurrence_weekday = static_cast<int>(IntValue(object, "recurrenceWeekday"));
    event.recurrence_month_day = static_cast<int>(IntValue(object, "recurrenceMonthDay"));
    event.reminder_offset_minutes = static_cast<int>(IntValue(object, "reminderOffsetMinutes"));
    event.weak_reminder_enabled = BoolValue(object, "weakReminderEnabled");
    event.weak_reminder_minutes = static_cast<int>(IntValue(object, "weakReminderMinutes", 15));
    event.paused = BoolValue(object, "paused");
    event.terminated = BoolValue(object, "terminated");
    const cJSON* skipped = cJSON_GetObjectItemCaseSensitive(object, "skippedOccurrences");
    cJSON* value = nullptr;
    cJSON_ArrayForEach(value, skipped) {
        if (cJSON_IsString(value) && value->valuestring != nullptr) event.skipped_occurrences.emplace_back(value->valuestring);
    }
    event.created_at = IntValue(object, "createdAt");
    event.updated_at = IntValue(object, "updatedAt", event.created_at);
    return event;
}

cJSON* ReminderToJson(const Reminder& reminder) {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "id", reminder.id);
    AddString(object, "eventId", reminder.event_id);
    AddString(object, "originalStartAt", reminder.original_start_at);
    AddString(object, "triggerAt", reminder.trigger_at);
    cJSON_AddBoolToObject(object, "weak", reminder.weak);
    AddString(object, "status", ReminderStatusName(reminder.status));
    cJSON_AddNumberToObject(object, "snoozeCount", reminder.snooze_count);
    cJSON_AddNumberToObject(object, "deliveredAt", static_cast<double>(reminder.delivered_at));
    cJSON_AddNumberToObject(object, "closedAt", static_cast<double>(reminder.closed_at));
    AddString(object, "imReportedTriggerAt", reminder.im_reported_trigger_at);
    return object;
}

Reminder ReminderFromJson(const cJSON* object) {
    Reminder reminder;
    if (const char* value = StringValue(object, "id")) reminder.id = value;
    if (const char* value = StringValue(object, "eventId")) reminder.event_id = value;
    if (const char* value = StringValue(object, "originalStartAt")) reminder.original_start_at = value;
    if (const char* value = StringValue(object, "triggerAt")) reminder.trigger_at = value;
    reminder.weak = BoolValue(object, "weak");
    if (const char* value = StringValue(object, "status")) reminder.status = ReminderStatusFromName(value);
    reminder.snooze_count = static_cast<int>(IntValue(object, "snoozeCount"));
    reminder.delivered_at = IntValue(object, "deliveredAt");
    reminder.closed_at = IntValue(object, "closedAt");
    if (const char* value = StringValue(object, "imReportedTriggerAt")) {
        reminder.im_reported_trigger_at = value;
    }
    return reminder;
}

cJSON* NoteToJson(const ShortNote& note) {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "id", note.id);
    AddString(object, "content", note.content);
    AddString(object, "category", note.category);
    cJSON_AddNumberToObject(object, "expiresAt", static_cast<double>(note.expires_at));
    cJSON_AddNumberToObject(object, "createdAt", static_cast<double>(note.created_at));
    return object;
}

ShortNote NoteFromJson(const cJSON* object) {
    ShortNote note;
    if (const char* value = StringValue(object, "id")) note.id = value;
    if (const char* value = StringValue(object, "content")) note.content = value;
    if (const char* value = StringValue(object, "category")) note.category = value;
    note.expires_at = IntValue(object, "expiresAt");
    note.created_at = IntValue(object, "createdAt");
    return note;
}

cJSON* ReceiptToJson(const Receipt& receipt) {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "id", receipt.id);
    AddString(object, "type", receipt.type);
    AddString(object, "speech", receipt.speech);
    cJSON_AddNumberToObject(object, "createdAt", static_cast<double>(receipt.created_at));
    return object;
}

Receipt ReceiptFromJson(const cJSON* object) {
    Receipt receipt;
    if (const char* value = StringValue(object, "id")) receipt.id = value;
    if (const char* value = StringValue(object, "type")) receipt.type = value;
    if (const char* value = StringValue(object, "speech")) receipt.speech = value;
    receipt.created_at = IntValue(object, "createdAt");
    return receipt;
}

cJSON* UndoToJson(const UndoOperation& operation) {
    cJSON* object = cJSON_CreateObject();
    AddString(object, "id", operation.id);
    AddString(object, "action", operation.action);
    AddString(object, "eventSnapshot", operation.event_snapshot);
    AddString(object, "reminderSnapshot", operation.reminder_snapshot);
    cJSON* reminders = cJSON_AddArrayToObject(object, "reminderSnapshots");
    for (const auto& snapshot : operation.reminder_snapshots) {
        cJSON_AddItemToArray(reminders, cJSON_CreateString(snapshot.c_str()));
    }
    cJSON_AddNumberToObject(object, "expiresAt", static_cast<double>(operation.expires_at));
    cJSON_AddBoolToObject(object, "undone", operation.undone);
    cJSON_AddNumberToObject(object, "createdAt", static_cast<double>(operation.created_at));
    return object;
}

UndoOperation UndoFromJson(const cJSON* object) {
    UndoOperation operation;
    if (const char* value = StringValue(object, "id")) operation.id = value;
    if (const char* value = StringValue(object, "action")) operation.action = value;
    if (const char* value = StringValue(object, "eventSnapshot")) operation.event_snapshot = value;
    if (const char* value = StringValue(object, "reminderSnapshot")) operation.reminder_snapshot = value;
    const cJSON* snapshots = cJSON_GetObjectItemCaseSensitive(object, "reminderSnapshots");
    cJSON* snapshot = nullptr;
    cJSON_ArrayForEach(snapshot, snapshots) {
        if (cJSON_IsString(snapshot) && snapshot->valuestring != nullptr) {
            operation.reminder_snapshots.emplace_back(snapshot->valuestring);
        }
    }
    // State written by the first MVP only had one reminderSnapshot. Keep it in
    // the new vector so undo remains lossless after an upgrade.
    if (operation.reminder_snapshots.empty() && !operation.reminder_snapshot.empty()) {
        operation.reminder_snapshots.push_back(operation.reminder_snapshot);
    }
    operation.expires_at = IntValue(object, "expiresAt");
    operation.undone = BoolValue(object, "undone");
    operation.created_at = IntValue(object, "createdAt");
    return operation;
}

cJSON* StateToJson(const State& state) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schemaVersion", 2);
    cJSON* events = cJSON_AddArrayToObject(root, "events");
    for (const auto& event : state.events) cJSON_AddItemToArray(events, EventToJson(event));
    cJSON* reminders = cJSON_AddArrayToObject(root, "reminders");
    for (const auto& reminder : state.reminders) cJSON_AddItemToArray(reminders, ReminderToJson(reminder));
    cJSON* notes = cJSON_AddArrayToObject(root, "notes");
    for (const auto& note : state.notes) cJSON_AddItemToArray(notes, NoteToJson(note));
    cJSON* receipts = cJSON_AddArrayToObject(root, "receipts");
    for (const auto& receipt : state.receipts) cJSON_AddItemToArray(receipts, ReceiptToJson(receipt));
    cJSON* undo = cJSON_AddArrayToObject(root, "undoOperations");
    for (const auto& operation : state.undo_operations) cJSON_AddItemToArray(undo, UndoToJson(operation));
    cJSON* processed_actions = cJSON_AddArrayToObject(root, "processedImActions");
    for (const auto& action : state.processed_im_actions) {
        cJSON* item = cJSON_CreateObject();
        AddString(item, "actionId", action.action_id);
        cJSON_AddBoolToObject(item, "ok", action.ok);
        AddString(item, "resultJson", action.result_json);
        cJSON_AddNumberToObject(item, "processedAt", static_cast<double>(action.processed_at));
        cJSON_AddItemToArray(processed_actions, item);
    }
    return root;
}

bool ParseState(const char* text, State* state) {
    cJSON* root = cJSON_Parse(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    state->events.clear();
    state->reminders.clear();
    state->notes.clear();
    state->receipts.clear();
    state->undo_operations.clear();
    state->processed_im_actions.clear();
    const cJSON* item = nullptr;
    const cJSON* array = cJSON_GetObjectItemCaseSensitive(root, "events");
    cJSON_ArrayForEach(item, array) { if (cJSON_IsObject(item)) state->events.push_back(EventFromJson(item)); }
    array = cJSON_GetObjectItemCaseSensitive(root, "reminders");
    cJSON_ArrayForEach(item, array) { if (cJSON_IsObject(item)) state->reminders.push_back(ReminderFromJson(item)); }
    array = cJSON_GetObjectItemCaseSensitive(root, "notes");
    cJSON_ArrayForEach(item, array) { if (cJSON_IsObject(item)) state->notes.push_back(NoteFromJson(item)); }
    array = cJSON_GetObjectItemCaseSensitive(root, "receipts");
    cJSON_ArrayForEach(item, array) { if (cJSON_IsObject(item)) state->receipts.push_back(ReceiptFromJson(item)); }
    array = cJSON_GetObjectItemCaseSensitive(root, "undoOperations");
    cJSON_ArrayForEach(item, array) { if (cJSON_IsObject(item)) state->undo_operations.push_back(UndoFromJson(item)); }
    array = cJSON_GetObjectItemCaseSensitive(root, "processedImActions");
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) continue;
        ProcessedImAction action;
        if (const char* value = StringValue(item, "actionId")) action.action_id = value;
        action.ok = BoolValue(item, "ok");
        if (const char* value = StringValue(item, "resultJson")) action.result_json = value;
        action.processed_at = IntValue(item, "processedAt");
        if (!action.action_id.empty()) state->processed_im_actions.push_back(std::move(action));
    }
    cJSON_Delete(root);
    return true;
}

ReadResult ReadStateFile(const char* path, State* state) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return errno == ENOENT ? ReadResult::Missing : ReadResult::Invalid;

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 512 * 1024) {
        std::fclose(file);
        return ReadResult::Invalid;
    }

    std::string text(static_cast<size_t>(size), '\0');
    const size_t read = std::fread(text.data(), 1, text.size(), file);
    std::fclose(file);
    if (read != text.size() || !ParseState(text.c_str(), state)) return ReadResult::Invalid;
    return ReadResult::Loaded;
}

}  // namespace

FlashStorage::~FlashStorage() {
    if (initialized_) esp_vfs_spiffs_unregister(kPartitionLabel);
}

bool FlashStorage::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;
    const esp_vfs_spiffs_conf_t config = {
        .base_path = kMountPoint,
        .partition_label = kPartitionLabel,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t result = esp_vfs_spiffs_register(&config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount data partition: %s", esp_err_to_name(result));
        return false;
    }
    initialized_ = true;
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(kPartitionLabel, &total, &used) == ESP_OK) {
        ESP_LOGI(kTag, "Mounted %s: %u/%u bytes", kPartitionLabel,
                 static_cast<unsigned>(used), static_cast<unsigned>(total));
    }
    return true;
}

bool FlashStorage::Load(State* state) {
    if (state == nullptr || !Initialize()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    ReadResult result = ReadStateFile(kStatePath, state);
    bool saw_invalid = result == ReadResult::Invalid;
    const char* loaded_path = kStatePath;
    if (result != ReadResult::Loaded) {
        result = ReadStateFile(kBackupPath, state);
        saw_invalid = saw_invalid || result == ReadResult::Invalid;
        loaded_path = kBackupPath;
    }
    if (result != ReadResult::Loaded) {
        result = ReadStateFile(kTempPath, state);
        saw_invalid = saw_invalid || result == ReadResult::Invalid;
        loaded_path = kTempPath;
    }
    if (result != ReadResult::Loaded) {
        if (!saw_invalid) {
            *state = State{};
            return true;
        }
        ESP_LOGW(kTag, "No valid state journal found");
        *state = State{};
        return false;
    }
    if (std::strcmp(loaded_path, kStatePath) != 0) {
        ESP_LOGW(kTag, "Recovered state journal from %s", loaded_path);
    }
    ESP_LOGI(kTag, "Loaded %u events, %u reminders, %u notes",
             static_cast<unsigned>(state->events.size()),
             static_cast<unsigned>(state->reminders.size()),
             static_cast<unsigned>(state->notes.size()));
    return true;
}

bool FlashStorage::Save(const State& state) {
    if (!Initialize()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = StateToJson(state);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) return false;
    // SPIFFS can reject a second rename in the same transaction with a generic
    // EIO. Rotate the old journal first, then write the new journal directly;
    // the rotated copy remains available as a power-loss fallback.
    const bool has_current = access(kStatePath, F_OK) == 0;
    if (has_current) {
        if (std::remove(kBackupPath) != 0 && errno != ENOENT) {
            const int error = errno;
            cJSON_free(text);
            ESP_LOGE(kTag, "Cannot rotate state backup: %s", std::strerror(error));
            return false;
        }
        if (std::rename(kStatePath, kBackupPath) != 0) {
            const int error = errno;
            cJSON_free(text);
            ESP_LOGE(kTag, "Cannot preserve previous state journal: %s", std::strerror(error));
            return false;
        }
    }

    FILE* file = std::fopen(kStatePath, "wb");
    if (file == nullptr) {
        const int error = errno;
        cJSON_free(text);
        if (has_current && std::rename(kBackupPath, kStatePath) != 0) {
            ESP_LOGE(kTag, "Cannot restore previous state journal: %s", std::strerror(errno));
        }
        ESP_LOGE(kTag, "Cannot open state journal for writing: %s", std::strerror(error));
        return false;
    }

    const size_t length = std::strlen(text);
    const size_t written = std::fwrite(text, 1, length, file);
    const int flush_result = std::fflush(file);
    const int sync_result = flush_result == 0 ? fsync(fileno(file)) : -1;
    const int close_result = std::fclose(file);
    const int io_error = errno == 0 ? EIO : errno;
    cJSON_free(text);
    if (written != length || flush_result != 0 || sync_result != 0 || close_result != 0) {
        const int restore_error = errno;
        std::remove(kStatePath);
        if (has_current && std::rename(kBackupPath, kStatePath) != 0) {
            ESP_LOGE(kTag, "Cannot restore previous state journal: %s", std::strerror(errno));
        }
        ESP_LOGE(kTag, "Cannot flush state journal: %s", std::strerror(io_error));
        errno = restore_error;
        return false;
    }

    // Remove a stale temp journal left by an older firmware version. Failure
    // here is harmless because Load() still prefers the committed state file.
    std::remove(kTempPath);
    return true;
}

}  // namespace voicelife
