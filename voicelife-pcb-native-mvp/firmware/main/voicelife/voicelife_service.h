#pragma once

#include "voicelife_domain.h"
#include "voicelife_storage.h"

#include <cJSON.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace voicelife {

class VoiceLifeService {
public:
    using SpeechCallback = std::function<bool(const std::string&)>;
    using Clock = std::function<int64_t()>;

    // A replaceable storage and clock keep host/service tests deterministic
    // without changing the firmware's default SPIFFS-backed behavior.
    explicit VoiceLifeService(Storage* storage = nullptr, Clock clock = {});
    ~VoiceLifeService() = default;

    bool Initialize();
    void SetSpeechCallback(SpeechCallback callback);

    cJSON* CalendarCreate(const cJSON* arguments);
    cJSON* CalendarQuery(const cJSON* arguments);
    cJSON* CalendarFind(const cJSON* arguments);
    cJSON* CalendarModify(const cJSON* arguments);
    cJSON* CalendarSkipOccurrence(const cJSON* arguments);
    cJSON* CalendarPauseSeries(const cJSON* arguments);
    cJSON* CalendarResumeSeries(const cJSON* arguments);
    cJSON* CalendarTerminateSeries(const cJSON* arguments);
    cJSON* CalendarDelete(const cJSON* arguments);
    cJSON* CalendarUndo(const cJSON* arguments);

    cJSON* ReminderListDue(const cJSON* arguments);
    cJSON* ReminderClose(const cJSON* arguments);
    cJSON* ReminderSnooze(const cJSON* arguments);
    cJSON* ReminderGetDetails(const cJSON* arguments);

    cJSON* NoteRecord(const cJSON* arguments);
    cJSON* NoteQuery(const cJSON* arguments);

    // The IM bridge deliberately speaks JSON strings at this boundary. This
    // keeps the scheduling domain independent from HTTP/WebSocket code and
    // makes retries and action receipts host-testable.
    std::vector<std::string> CollectPendingImNotifications(const std::string& device_id);
    bool MarkImNotificationReported(const std::string& reminder_id, const std::string& trigger_at);
    std::string ApplyImAction(const std::string& action_id, const std::string& type,
                              const std::string& reminder_id, int minutes = 0);

    // Called from the application clock tick. It never touches the audio task.
    void Tick();

private:
    FlashStorage flash_storage_;
    Storage* storage_ = nullptr;
    Clock clock_;
    State state_;
    SpeechCallback speech_callback_;
    mutable std::mutex mutex_;
    bool initialized_ = false;

    cJSON* Error(const std::string& message) const;
    cJSON* Result(const std::string& speech) const;
    bool SaveLocked();
    cJSON* CommitLocked(cJSON* success_result, const char* operation);
    Event* FindEventLocked(const std::string& event_id);
    const Event* FindEventLocked(const std::string& event_id) const;
    Reminder* FindReminderLocked(const std::string& reminder_id);
    const Reminder* FindReminderLocked(const std::string& reminder_id) const;
    const Event* FindEquivalentEventLocked(const Event& candidate) const;
    std::string EventJson(const Event& event) const;
    std::string ReminderJson(const Reminder& reminder) const;
    void AddReceiptLocked(const char* type, const std::string& speech, cJSON* result);
    void AddUndoLocked(const char* action, const Event* event, const Reminder* reminder, cJSON* result,
                       const std::vector<Reminder>* reminders = nullptr);
    bool HasConflictLocked(const Event& candidate, std::vector<const Event*>* conflicts) const;
    std::string ConflictToken(const Event& candidate, const std::vector<const Event*>& conflicts) const;
    void EnumerateOccurrencesLocked(const Event& event, int64_t range_start, int64_t range_end,
                                    std::vector<std::string>* occurrences) const;
    cJSON* EventToPublicJson(const Event& event) const;
    cJSON* ReminderToPublicJson(const Reminder& reminder, const Event* event) const;
    cJSON* OccurrenceToPublicJson(const Event& event, const std::string& original_start_at) const;
    cJSON* OccurrenceToQueryJson(const Event& event, const std::string& original_start_at) const;
    bool IsOccurrenceSkippedLocked(const Event& event, const std::string& occurrence_start_at) const;
    bool IsOccurrenceInSeriesLocked(const Event& event, const std::string& occurrence_start_at) const;
    void EnsureRemindersForOccurrenceLocked(const Event& event, const std::string& original_start_at,
                                             bool only_if_future);
    void EnsureNextReminderLocked(const Event& event, const std::string& after_original_start_at);
    std::string ApplyImActionLocked(const std::string& action_id, const std::string& type,
                                    const std::string& reminder_id, int minutes);
    void PruneLocked(int64_t now);
    int64_t Now() const;
};

}  // namespace voicelife
