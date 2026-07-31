#include "voicelife_service.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voicelife {

// The host runner uses an in-memory Storage. These definitions satisfy the
// default FlashStorage vtable without linking ESP-IDF SPIFFS code.
FlashStorage::~FlashStorage() = default;
bool FlashStorage::Initialize() { return false; }
bool FlashStorage::Load(State*) { return false; }
bool FlashStorage::Save(const State&) { return false; }

}  // namespace voicelife

namespace {

using voicelife::State;
using voicelife::Storage;
using voicelife::VoiceLifeService;

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

void Check(bool condition, const char* expression, int line) {
    if (!condition) throw TestFailure(std::string("line ") + std::to_string(line) + ": " + expression);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

class MemoryStorage final : public Storage {
public:
    bool Initialize() override {
        initialized = true;
        return true;
    }

    bool Load(State* state) override {
        if (!initialized || state == nullptr) return false;
        if (fail_next_load) {
            fail_next_load = false;
            return false;
        }
        if (has_state) *state = state_copy;
        else *state = State{};
        return true;
    }

    bool Save(const State& state) override {
        if (!initialized) return false;
        if (fail_next_save) {
            fail_next_save = false;
            return false;
        }
        state_copy = state;
        has_state = true;
        ++save_count;
        return true;
    }

    bool initialized = false;
    bool has_state = false;
    bool fail_next_load = false;
    bool fail_next_save = false;
    int save_count = 0;
    State state_copy;
};

struct Json final {
    cJSON* value = nullptr;
    explicit Json(cJSON* json) : value(json) {}
    ~Json() { cJSON_Delete(value); }
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;
    Json(Json&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Json& operator=(Json&& other) noexcept {
        if (this != &other) {
            cJSON_Delete(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
};

Json Call(VoiceLifeService& service,
          cJSON* (VoiceLifeService::*method)(const cJSON*),
          const std::string& arguments = "{}") {
    cJSON* input = cJSON_Parse(arguments.c_str());
    CHECK(input != nullptr);
    Json result{(service.*method)(input)};
    cJSON_Delete(input);
    CHECK(result.value != nullptr);
    return result;
}

bool Bool(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsBool(value));
    return cJSON_IsTrue(value);
}

double Number(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsNumber(value));
    return value->valuedouble;
}

std::string String(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsString(value) && value->valuestring != nullptr);
    return value->valuestring;
}

int ArraySize(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsArray(value));
    return cJSON_GetArraySize(value);
}

const cJSON* ArrayItem(const cJSON* object, const char* key, int index) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsArray(value));
    const cJSON* item = cJSON_GetArrayItem(value, index);
    CHECK(item != nullptr);
    return item;
}

void ExpectOk(const Json& result) { CHECK(Bool(result.value, "ok")); }
void ExpectError(const Json& result) { CHECK(!Bool(result.value, "ok")); }

struct Fixture final {
    static constexpr int64_t kInitialNow = 1785542400;  // 2026-08-01T00:00:00Z

    Fixture()
        : service(&storage, [this]() { return now; }) {
        CHECK(service.Initialize());
        service.SetSpeechCallback([this](const std::string& text) {
            announcements.push_back(text);
            return speech_succeeds;
        });
    }

    std::string At(int64_t delta_seconds) const {
        return voicelife::FormatUtc(now + delta_seconds);
    }

    int64_t now = kInitialNow;
    MemoryStorage storage;
    VoiceLifeService service;
    bool speech_succeeds = true;
    std::vector<std::string> announcements;
};

void TestIso8601() {
    using voicelife::FormatUtc;
    using voicelife::FormatSpokenTime;
    using voicelife::FormatSpokenTimeRange;
    using voicelife::ParseIso8601;

    const int64_t utc = ParseIso8601("2026-08-01T12:34:56Z");
    CHECK(utc >= 0);
    CHECK(utc == ParseIso8601("2026-08-01T20:34:56+08:00"));
    CHECK(utc == ParseIso8601("2026-08-01T07:04:56-05:30"));
    CHECK(utc == ParseIso8601("2026-08-01 20:34:56+0800"));
    CHECK(utc == ParseIso8601("2026-08-01T12:34:56.999999Z"));
    CHECK(ParseIso8601("2024-02-29T23:59:59+08:00") >= 0);
    CHECK(FormatUtc(utc) == "2026-08-01T12:34:56Z");

    const int64_t thursday_noon = ParseIso8601("2026-07-30T12:00:00+08:00");
    CHECK(voicelife::AlignWeeklyStartAt("2026-08-07T17:00:00+08:00", 5, thursday_noon) ==
          "2026-07-31T17:00:00+08:00");
    CHECK(voicelife::AlignWeeklyStartAt("2026-08-01T17:00:00+08:00", 5, thursday_noon) ==
          "2026-07-31T17:00:00+08:00");
    CHECK(voicelife::AlignWeeklyStartAt("2026-07-31T17:00:00+08:00", 5,
                                       ParseIso8601("2026-07-31T18:00:00+08:00")) ==
          "2026-08-07T17:00:00+08:00");

    const std::vector<std::string> invalid = {
        "2026-02-29T00:00:00Z", "2024-02-30T00:00:00Z", "2026-08-01T24:00:00Z",
        "2026-08-01T12:60:00Z", "2026-08-01T12:34:60Z", "2026-08-01T12:34:56",
        "2026-08-01T12:34:56+24:00", "2026-08-01T12:34:56+08:60", "not-a-time",
    };
    for (const auto& value : invalid) CHECK(ParseIso8601(value) < 0);

    const int64_t now = ParseIso8601("2026-07-30T11:39:00+08:00");
    CHECK(FormatSpokenTime("2026-07-30T20:00:00+08:00", now) == "今晚8点");
    CHECK(FormatSpokenTime("2026-07-31T09:30:00+08:00", now) == "明天上午9点半");
    CHECK(FormatSpokenTime("2026-07-30T12:00:00Z", now) == "今晚8点");
    CHECK(FormatSpokenTimeRange("2026-07-30T20:00:00+08:00",
                                "2026-07-30T21:00:00+08:00", now) ==
          "今晚8点到9点");
    CHECK(FormatSpokenTimeRange("2026-07-30T11:00:00+08:00",
                                "2026-07-30T13:00:00+08:00", now) ==
          "今天上午11点到下午1点");
    CHECK(FormatSpokenTimeRange("2026-07-30T23:00:00+08:00",
                                "2026-07-31T01:00:00+08:00", now) ==
          "今晚11点到明天凌晨1点");
}

void TestConciseSpokenOutputAndQueryLimit() {
    Fixture fixture;
    fixture.now = voicelife::ParseIso8601("2026-07-30T11:39:00+08:00");

    Json first = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      R"({"title":"开会","startsAt":"2026-07-30T20:00:00+08:00"})");
    ExpectOk(first);
    CHECK(String(first.value, "speech") == "已创建开会，今晚8点。");

    ExpectOk(Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        R"({"title":"复盘","startsAt":"2026-07-30T21:00:00+08:00","kind":"time_block","durationMinutes":60})"));
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  R"({"title":"散步","startsAt":"2026-07-30T22:30:00+08:00"})"));

    Json query = Call(
        fixture.service, &VoiceLifeService::CalendarQuery,
        R"({"rangeStart":"2026-07-30T00:00:00+08:00","rangeEnd":"2026-07-31T00:00:00+08:00"})");
    ExpectOk(query);
    CHECK(Number(query.value, "total") == 3);
    CHECK(Number(query.value, "returned") == 2);
    CHECK(Bool(query.value, "truncated"));
    CHECK(ArraySize(query.value, "occurrences") == 2);
    const std::string speech = String(query.value, "speech");
    CHECK(speech ==
          "共有3条安排：今晚8点，开会；今晚9点到10点，复盘。"
          "其余日程我就不逐条念了，详细信息可在 IM 中查看。");
    CHECK(speech.find('T') == std::string::npos);
    CHECK(speech.find('+') == std::string::npos);

    fixture.now = voicelife::ParseIso8601("2026-07-30T20:00:00+08:00");
    fixture.service.Tick();
    CHECK(!fixture.announcements.empty());
    CHECK(fixture.announcements.back().find("提醒：开会到时间了。") != std::string::npos);
    CHECK(fixture.announcements.back().find('T') == std::string::npos);
}

void TestCalendarQuerySpeechBoundaries() {
    Fixture fixture;
    const std::string range_start = fixture.At(0);
    const std::string range_end = fixture.At(4 * 3600);
    const auto query = [&fixture, &range_start, &range_end]() {
        return Call(fixture.service, &VoiceLifeService::CalendarQuery,
                    "{\"rangeStart\":\"" + range_start + "\",\"rangeEnd\":\"" + range_end + "\"}");
    };
    constexpr char kImTail[] = "其余日程我就不逐条念了，详细信息可在 IM 中查看。";

    Json zero = query();
    ExpectOk(zero);
    CHECK(Number(zero.value, "total") == 0);
    CHECK(!Bool(zero.value, "truncated"));
    CHECK(String(zero.value, "speech").find(kImTail) == std::string::npos);

    for (int i = 1; i <= 3; ++i) {
        ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"事项" + std::to_string(i) + "\",\"startsAt\":\"" +
                          fixture.At(i * 3600) + "\"}"));
        Json result = query();
        ExpectOk(result);
        CHECK(Number(result.value, "total") == i);
        CHECK(Bool(result.value, "truncated") == (i > 2));
        const bool has_im_tail = String(result.value, "speech").find(kImTail) != std::string::npos;
        CHECK(has_im_tail == (i > 2));
    }
}

void TestExactDuplicateCreateIsIdempotent() {
    Fixture fixture;
    fixture.now = voicelife::ParseIso8601("2026-07-30T15:00:00+08:00");

    Json first = Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        R"({"title":"开会","startsAt":"2026-07-30T19:00:00+08:00","kind":"time_block","durationMinutes":30,"weakReminder":false})");
    ExpectOk(first);
    const std::string event_id = String(first.value, "eventId");
    CHECK(!event_id.empty());

    Json duplicate = Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        R"({"title":"开会","startsAt":"2026-07-30T11:00:00Z","endsAt":"2026-07-30T11:30:00Z","kind":"time_block","weakReminder":false})");
    ExpectOk(duplicate);
    CHECK(Bool(duplicate.value, "alreadyExists"));
    CHECK(String(duplicate.value, "speech") == "这条日程已经存在。");
    CHECK(String(duplicate.value, "eventId") == event_id);

    Json query = Call(
        fixture.service, &VoiceLifeService::CalendarQuery,
        R"({"rangeStart":"2026-07-30T19:00:00+08:00","rangeEnd":"2026-07-30T19:01:00+08:00"})");
    ExpectOk(query);
    CHECK(Number(query.value, "total") == 1);
    CHECK(ArraySize(query.value, "occurrences") == 1);

    fixture.now = voicelife::ParseIso8601("2026-07-30T19:00:00+08:00");
    fixture.service.Tick();
    CHECK(fixture.announcements.size() == 1);
}

void TestQueryBoundariesAndFindValidation() {
    Fixture fixture;
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"跨区间课程\",\"startsAt\":\"" + fixture.At(3600) +
                      "\",\"kind\":\"time_block\",\"durationMinutes\":60}"));
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"右边界事项\",\"startsAt\":\"" + fixture.At(7200) + "\"}"));

    Json before = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                       "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" +
                           fixture.At(3600) + "\"}");
    ExpectOk(before);
    CHECK(Number(before.value, "total") == 0);

    Json exact = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                      "{\"rangeStart\":\"" + fixture.At(3600) + "\",\"rangeEnd\":\"" +
                          fixture.At(7200) + "\"}");
    ExpectOk(exact);
    CHECK(Number(exact.value, "total") == 1);
    CHECK(String(ArrayItem(exact.value, "occurrences", 0), "title") == "跨区间课程");

    Json overlap = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                        "{\"rangeStart\":\"" + fixture.At(4000) + "\",\"rangeEnd\":\"" +
                            fixture.At(5000) + "\"}");
    ExpectOk(overlap);
    CHECK(Number(overlap.value, "total") == 1);

    ExpectError(Call(
        fixture.service, &VoiceLifeService::CalendarQuery,
        "{\"rangeStart\":\"" + fixture.At(7200) + "\",\"rangeEnd\":\"" + fixture.At(3600) + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarFind,
                     "{\"query\":\"课程\",\"rangeStart\":\"" + fixture.At(0) + "\"}"));
}

void TestCalendarFindPayloadIsBounded() {
    Fixture fixture;
    fixture.now = voicelife::ParseIso8601("2026-07-30T12:00:00+08:00");

    for (int hour = 18; hour < 23; ++hour) {
        ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"开会\",\"startsAt\":\"2026-07-30T" + std::to_string(hour) +
                          ":00:00+08:00\"}"));
    }

    Json result = Call(
        fixture.service, &VoiceLifeService::CalendarFind,
        R"({"query":"开会","rangeStart":"2026-07-30T00:00:00+08:00","rangeEnd":"2026-07-31T00:00:00+08:00"})");
    ExpectOk(result);
    CHECK(Number(result.value, "total") == 5);
    CHECK(Number(result.value, "returned") == 3);
    CHECK(Bool(result.value, "truncated"));
    CHECK(Bool(result.value, "requiresDisambiguation"));
    CHECK(ArraySize(result.value, "candidates") == 3);

    for (int index = 0; index < 3; ++index) {
        const cJSON* candidate = ArrayItem(result.value, "candidates", index);
        CHECK(!String(candidate, "eventId").empty());
        CHECK(String(candidate, "title") == "开会");
        CHECK(!String(candidate, "startsAt").empty());
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "originalStartAt") != nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "id") == nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "kind") == nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "timeZone") == nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "notes") == nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "recurrence") == nullptr);
        CHECK(cJSON_GetObjectItemCaseSensitive(candidate, "skippedOccurrences") == nullptr);
    }

    char* serialized = cJSON_PrintUnformatted(result.value);
    CHECK(serialized != nullptr);
    const size_t payload_size = std::strlen(serialized);
    cJSON_free(serialized);
    CHECK(payload_size < 1024);
}

void TestLeapDayAndMonthEndRecurrence() {
    {
        Fixture fixture;
        fixture.now = voicelife::ParseIso8601("2028-01-01T00:00:00+08:00");
        ExpectOk(Call(
            fixture.service, &VoiceLifeService::CalendarCreate,
            R"({"title":"闰日连续测试","startsAt":"2028-02-28T09:00:00+08:00","recurrence":{"frequency":"daily"}})"));
        Json leap = Call(
            fixture.service, &VoiceLifeService::CalendarQuery,
            R"({"rangeStart":"2028-02-28T00:00:00+08:00","rangeEnd":"2028-03-02T00:00:00+08:00"})");
        ExpectOk(leap);
        CHECK(Number(leap.value, "total") == 3);
        CHECK(String(ArrayItem(leap.value, "occurrences", 1), "startsAt") ==
              "2028-02-29T01:00:00Z");
    }
    {
        Fixture fixture;
        fixture.now = voicelife::ParseIso8601("2028-01-01T00:00:00+08:00");
        ExpectOk(Call(
            fixture.service, &VoiceLifeService::CalendarCreate,
            R"({"title":"月末盘点 2028","startsAt":"2028-01-31T20:00:00+08:00","recurrence":{"frequency":"monthly","monthDay":31}})"));
        Json month_end = Call(
            fixture.service, &VoiceLifeService::CalendarQuery,
            R"({"rangeStart":"2028-01-01T00:00:00+08:00","rangeEnd":"2028-04-01T00:00:00+08:00"})");
        ExpectOk(month_end);
        CHECK(Number(month_end.value, "total") == 2);
    }
}

void TestCalendarCrudAndQuery() {
    Fixture fixture;
    const std::string start = fixture.At(3600);
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"设计评审\",\"startsAt\":\"" + start +
                            "\",\"location\":\"会议室\",\"notes\":\"准备方案\"}");
    ExpectOk(created);
    const std::string event_id = String(created.value, "eventId");
    const std::string undo_id = String(created.value, "undoOperationId");
    CHECK(!event_id.empty() && !undo_id.empty());

    Json query = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                      "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" +
                          fixture.At(7200) + "\"}");
    ExpectOk(query);
    CHECK(Number(query.value, "total") == 1);
    const cJSON* occurrence = ArrayItem(query.value, "occurrences", 0);
    CHECK(String(occurrence, "eventId") == event_id);
    CHECK(String(occurrence, "title") == "设计评审");
    CHECK(String(occurrence, "location") == "会议室");

    Json find = Call(fixture.service, &VoiceLifeService::CalendarFind,
                     "{\"query\":\"评审\"}");
    ExpectOk(find);
    CHECK(Number(find.value, "total") == 1);

    Json modified =
        Call(fixture.service, &VoiceLifeService::CalendarModify,
             "{\"eventId\":\"" + event_id + "\",\"title\":\"架构评审\",\"newStartAt\":\"" +
                 fixture.At(5400) + "\"}");
    ExpectOk(modified);
    CHECK(String(cJSON_GetObjectItemCaseSensitive(modified.value, "event"), "title") == "架构评审");

    Json deleted_without_confirmation = Call(fixture.service, &VoiceLifeService::CalendarDelete,
                                             "{\"eventId\":\"" + event_id + "\"}");
    ExpectError(deleted_without_confirmation);
    CHECK(Bool(deleted_without_confirmation.value, "requiresConfirmation"));
    CHECK(String(deleted_without_confirmation.value, "speech") == "是否确认删除这条日程？");
    Json deleted = Call(
        fixture.service, &VoiceLifeService::CalendarDelete,
        "{\"eventId\":\"" + event_id + "\",\"confirmationToken\":\"delete-" + event_id + "\"}");
    ExpectOk(deleted);
    Json restored =
        Call(fixture.service, &VoiceLifeService::CalendarUndo,
             "{\"undoOperationId\":\"" + String(deleted.value, "undoOperationId") + "\"}");
    ExpectOk(restored);
    Json after_restore = Call(fixture.service, &VoiceLifeService::CalendarFind,
                              "{\"query\":\"架构\"}");
    ExpectOk(after_restore);
    CHECK(Number(after_restore.value, "total") == 1);
}

void TestCalendarValidationAndConflicts() {
    Fixture fixture;
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate, "{}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"x\",\"startsAt\":\"bad\"}"));
    ExpectError(Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        "{\"title\":\"x\",\"startsAt\":\"" + fixture.At(3600) + "\",\"kind\":\"time_block\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"x\",\"startsAt\":\"" + fixture.At(3600) + "\",\"endsAt\":\"" +
                         fixture.At(3500) + "\"}"));
    ExpectError(
        Call(fixture.service, &VoiceLifeService::CalendarCreate,
             "{\"title\":\"x\",\"startsAt\":\"" + fixture.At(3600) + "\",\"kind\":\"unknown\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"x\",\"startsAt\":\"" + fixture.At(3600) +
                         "\",\"recurrence\":{\"frequency\":\"yearly\"}}"));

    Json first = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"会议\",\"startsAt\":\"" + fixture.At(3600) + "\"}");
    ExpectOk(first);
    Json conflict = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         "{\"title\":\"冲突会议\",\"startsAt\":\"" + fixture.At(3600) + "\"}");
    ExpectError(conflict);
    CHECK(String(conflict.value, "reason") == "calendar_conflict");
    CHECK(Bool(conflict.value, "requiresConfirmation"));
    CHECK(String(conflict.value, "speech") == "时间与已有日程冲突，是否仍要创建？");
    CHECK(ArraySize(conflict.value, "conflicts") == 1);
    const std::string token = String(conflict.value, "confirmationToken");
    Json confirmed = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                          "{\"title\":\"冲突会议\",\"startsAt\":\"" + fixture.At(3600) +
                              "\",\"conflictConfirmationToken\":\"" + token + "\"}");
    ExpectOk(confirmed);
    CHECK(Bool(confirmed.value, "conflictConfirmed"));

    Json block = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"占用\",\"startsAt\":\"" + fixture.At(7200) +
                          "\",\"kind\":\"time_block\",\"durationMinutes\":30}");
    ExpectOk(block);
    Json overlap = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"落入\",\"startsAt\":\"" + fixture.At(7500) + "\"}");
    ExpectError(overlap);
    Json adjacent = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         "{\"title\":\"相邻\",\"startsAt\":\"" + fixture.At(9000) + "\"}");
    ExpectOk(adjacent);
}

void TestConflictTokenBindingForCreateAndModify() {
    Fixture fixture;
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"占用时段\",\"startsAt\":\"" + fixture.At(3600) +
                      "\",\"kind\":\"time_block\",\"durationMinutes\":60}"));
    Json conflict = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         "{\"title\":\"冲突点\",\"startsAt\":\"" + fixture.At(5400) + "\"}");
    ExpectError(conflict);
    const std::string create_token = String(conflict.value, "confirmationToken");
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"冲突点\",\"startsAt\":\"" + fixture.At(5400) +
                         "\",\"conflictConfirmationToken\":\"wrong\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"改过标题\",\"startsAt\":\"" + fixture.At(5400) +
                         "\",\"conflictConfirmationToken\":\"" + create_token + "\"}"));
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"冲突点\",\"startsAt\":\"" + fixture.At(5400) +
                      "\",\"conflictConfirmationToken\":\"" + create_token + "\"}"));

    Json movable = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"待移动\",\"startsAt\":\"" + fixture.At(10800) + "\"}");
    Json occupied = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         "{\"title\":\"目标位置\",\"startsAt\":\"" + fixture.At(14400) + "\"}");
    ExpectOk(movable);
    ExpectOk(occupied);
    const std::string movable_id = String(movable.value, "eventId");
    Json modify_conflict =
        Call(fixture.service, &VoiceLifeService::CalendarModify,
             "{\"eventId\":\"" + movable_id + "\",\"newStartAt\":\"" + fixture.At(14400) + "\"}");
    ExpectError(modify_conflict);
    CHECK(String(modify_conflict.value, "speech") == "修改后的时间与已有日程冲突，是否仍要修改？");
    const std::string modify_token = String(modify_conflict.value, "confirmationToken");
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarModify,
                     "{\"eventId\":\"" + movable_id +
                         "\",\"title\":\"参数已改变\",\"newStartAt\":\"" + fixture.At(14400) +
                         "\",\"conflictConfirmationToken\":\"" + modify_token + "\"}"));
    Json modified =
        Call(fixture.service, &VoiceLifeService::CalendarModify,
             "{\"eventId\":\"" + movable_id + "\",\"newStartAt\":\"" + fixture.At(14400) +
                 "\",\"conflictConfirmationToken\":\"" + modify_token + "\"}");
    ExpectOk(modified);
}

void TestCreateContractValidation() {
    Fixture fixture;
    Json relative = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         R"({"title":"戴耳机","delayMinutes":1,"kind":"point"})");
    ExpectOk(relative);
    CHECK(voicelife::ParseIso8601(String(relative.value, "startsAt")) == fixture.now + 60);
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"二选一\",\"startsAt\":\"" + fixture.At(3600) +
                         "\",\"delayMinutes\":1}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     R"({"title":"延迟非法","delayMinutes":0})"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"过去\",\"startsAt\":\"" + fixture.At(-2) + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"提醒太晚\",\"startsAt\":\"" + fixture.At(3600) +
                         "\",\"remindAt\":\"" + fixture.At(3660) + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"提醒已过\",\"startsAt\":\"" + fixture.At(3600) +
                         "\",\"remindAt\":\"" + fixture.At(-2) + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"二选一\",\"startsAt\":\"" + fixture.At(3600) +
                         "\",\"endsAt\":\"" + fixture.At(7200) + "\",\"durationMinutes\":30}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"点事件\",\"startsAt\":\"" + fixture.At(3600) +
                         "\",\"endsAt\":\"" + fixture.At(7200) + "\",\"kind\":\"point\"}"));
    Json aligned_weekday = Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        "{\"title\":\"星期校正\",\"startsAt\":\"2026-08-03T09:00:00+08:00\"," 
        "\"recurrence\":{\"frequency\":\"weekly\",\"weekday\":2}}");
    ExpectOk(aligned_weekday);
    CHECK(voicelife::ParseIso8601(String(aligned_weekday.value, "startsAt")) ==
          voicelife::ParseIso8601("2026-08-04T09:00:00+08:00"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"非法星期\",\"startsAt\":\"2026-08-03T09:00:00+08:00\","
                     "\"recurrence\":{\"frequency\":\"weekly\",\"weekday\":8}}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                     "{\"title\":\"日期不符\",\"startsAt\":\"2026-08-31T09:00:00+08:00\","
                     "\"recurrence\":{\"frequency\":\"monthly\",\"monthDay\":30}}"));
}

void TestRecurringOccurrencesAndDelivery() {
    {
        Fixture fixture;
        ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"日报\",\"startsAt\":\"" + fixture.At(3600) +
                          "\",\"recurrence\":{\"frequency\":\"daily\"}}"));
        Json queried = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                            "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" +
                                fixture.At(3 * 86400) + "\"}");
        ExpectOk(queried);
        CHECK(Number(queried.value, "total") == 3);
    }
    {
        Fixture fixture;
        ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"周报\",\"startsAt\":\"2026-08-03T09:00:00+08:00\","
                      "\"recurrence\":{\"frequency\":\"weekly\",\"weekday\":1}}"));
        Json queried = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                            "{\"rangeStart\":\"2026-08-03T00:00:00+08:00\","
                            "\"rangeEnd\":\"2026-08-24T23:59:59+08:00\"}");
        ExpectOk(queried);
        CHECK(Number(queried.value, "total") == 4);
    }
    {
        Fixture fixture;
        ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"月末盘点\",\"startsAt\":\"2026-08-31T20:00:00+08:00\","
                      "\"recurrence\":{\"frequency\":\"monthly\",\"monthDay\":31}}"));
        Json queried = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                            "{\"rangeStart\":\"2026-08-01T00:00:00+08:00\","
                            "\"rangeEnd\":\"2026-11-01T00:00:00+08:00\"}");
        ExpectOk(queried);
        CHECK(Number(queried.value, "total") == 2);
        CHECK(String(ArrayItem(queried.value, "occurrences", 1), "startsAt") ==
              "2026-10-31T12:00:00Z");
    }
    {
        Fixture fixture;
        ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"每日服药\",\"startsAt\":\"" + fixture.At(1) +
                          "\",\"recurrence\":{\"frequency\":\"daily\"}}"));
        fixture.now += 2;
        fixture.service.Tick();
        CHECK(fixture.announcements.size() == 1);
        fixture.now += 86400;
        fixture.service.Tick();
        CHECK(fixture.announcements.size() == 2);
    }
}

void TestSkipOneRecurringOccurrence() {
    Fixture fixture;
    const std::string first_start = fixture.At(3600);
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"晨会\",\"startsAt\":\"" + first_start +
                            "\",\"recurrence\":{\"frequency\":\"daily\"}}" );
    ExpectOk(created);
    const std::string event_id = String(created.value, "eventId");
    Json skipped = Call(fixture.service, &VoiceLifeService::CalendarSkipOccurrence,
                        "{\"eventId\":\"" + event_id + "\",\"originalStartAt\":\"" + first_start + "\"}");
    ExpectOk(skipped);
    Json queried = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                        "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" +
                            fixture.At(3 * 86400) + "\"}");
    ExpectOk(queried);
    CHECK(Number(queried.value, "total") == 2);
    CHECK(String(ArrayItem(queried.value, "occurrences", 0), "startsAt") == fixture.At(3600 + 86400));
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                  "{\"undoOperationId\":\"" + String(skipped.value, "undoOperationId") + "\"}"));
    Json restored = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                         "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" +
                             fixture.At(3 * 86400) + "\"}");
    CHECK(Number(restored.value, "total") == 3);
}

void TestSingleEventSkipCancelsImmediatelyAndIsIdempotent() {
    Fixture fixture;
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"一次事项\",\"startsAt\":\"" + fixture.At(3600) + "\"}");
    ExpectOk(created);
    const std::string event_id = String(created.value, "eventId");
    Json skipped = Call(fixture.service, &VoiceLifeService::CalendarSkipOccurrence,
                        "{\"eventId\":\"" + event_id + "\"}");
    ExpectOk(skipped);
    CHECK(String(skipped.value, "speech") == "已取消一次事项。");
    Json duplicate = Call(fixture.service, &VoiceLifeService::CalendarSkipOccurrence,
                          "{\"eventId\":\"" + event_id + "\"}");
    ExpectOk(duplicate);
    CHECK(Bool(duplicate.value, "alreadySkipped"));

    Json queried = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                        "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" +
                            fixture.At(7200) + "\"}");
    ExpectOk(queried);
    CHECK(Number(queried.value, "total") == 0);
}

void TestSingleEventScopeAndAsrTolerantFind() {
    Fixture fixture;
    fixture.now = voicelife::ParseIso8601("2026-07-30T12:00:00+08:00");
    Json created = Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        R"({"title":"和 pz 开会","startsAt":"2026-07-30T18:00:00+08:00"})");
    ExpectOk(created);
    const std::string event_id = String(created.value, "eventId");

    Json spaced_letters = Call(
        fixture.service, &VoiceLifeService::CalendarFind,
        R"({"query":"P Z","rangeStart":"2026-07-30T18:00:00+08:00","rangeEnd":"2026-07-30T18:01:00+08:00"})");
    ExpectOk(spaced_letters);
    CHECK(Number(spaced_letters.value, "total") == 1);
    CHECK(!Bool(spaced_letters.value, "queryRelaxed"));
    CHECK(String(ArrayItem(spaced_letters.value, "candidates", 0), "eventId") == event_id);

    Json asr_substitution = Call(
        fixture.service, &VoiceLifeService::CalendarFind,
        R"({"query":"PC","rangeStart":"2026-07-30T18:00:00+08:00","rangeEnd":"2026-07-30T18:01:00+08:00"})");
    ExpectOk(asr_substitution);
    CHECK(Number(asr_substitution.value, "total") == 1);
    CHECK(Bool(asr_substitution.value, "queryRelaxed"));
    CHECK(String(ArrayItem(asr_substitution.value, "candidates", 0), "eventId") == event_id);

    Json modified = Call(
        fixture.service, &VoiceLifeService::CalendarModify,
        "{\"eventId\":\"" + event_id +
            "\",\"scope\":\"this_occurrence\",\"newStartAt\":\"2026-07-30T20:00:00+08:00\"}");
    ExpectOk(modified);
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarModify,
                     "{\"eventId\":\"" + event_id +
                         "\",\"scope\":\"this_and_future\",\"newStartAt\":\"2026-07-30T21:00:00+08:00\"}"));
}

void TestWeeklyCreateAlignsFirstOccurrenceLocally() {
    Fixture fixture;
    fixture.now = voicelife::ParseIso8601("2026-07-30T12:00:00+08:00");
    Json created = Call(
        fixture.service, &VoiceLifeService::CalendarCreate,
        R"({"title":"提交周报","startsAt":"2026-08-01T17:00:00+08:00","kind":"point","recurrence":{"frequency":"weekly","weekday":5}})");
    ExpectOk(created);
    CHECK(voicelife::ParseIso8601(String(created.value, "startsAt")) ==
          voicelife::ParseIso8601("2026-07-31T17:00:00+08:00"));

    Json found = Call(
        fixture.service, &VoiceLifeService::CalendarFind,
        R"({"query":"周报","rangeStart":"2026-07-31T17:00:00+08:00","rangeEnd":"2026-07-31T17:01:00+08:00"})");
    ExpectOk(found);
    CHECK(Number(found.value, "total") == 1);
    CHECK(voicelife::ParseIso8601(String(ArrayItem(found.value, "candidates", 0), "originalStartAt")) ==
          voicelife::ParseIso8601("2026-07-31T17:00:00+08:00"));
}

void TestReminderOffsetAndWeakReminderUndo() {
    Fixture fixture;
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"提前半小时\",\"startsAt\":\"" + fixture.At(3600) +
                            "\",\"remindAt\":\"" + fixture.At(1800) + "\"}");
    ExpectOk(created);
    const std::string event_id = String(created.value, "eventId");
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarModify,
                  "{\"eventId\":\"" + event_id + "\",\"newStartAt\":\"" + fixture.At(7200) + "\"}"));
    const auto main = std::find_if(fixture.storage.state_copy.reminders.begin(),
                                   fixture.storage.state_copy.reminders.end(),
                                   [&event_id](const auto& reminder) {
                                       return reminder.event_id == event_id && !reminder.weak;
                                   });
    CHECK(main != fixture.storage.state_copy.reminders.end());
    CHECK(main->trigger_at == fixture.At(5400));

    Json block = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                      "{\"title\":\"带弱提醒\",\"startsAt\":\"" + fixture.At(10800) +
                          "\",\"durationMinutes\":60}");
    ExpectOk(block);
    const std::string block_id = String(block.value, "eventId");
    Json challenge = Call(fixture.service, &VoiceLifeService::CalendarDelete,
                          "{\"eventId\":\"" + block_id + "\"}");
    const std::string token = String(challenge.value, "confirmationToken");
    Json deleted = Call(fixture.service, &VoiceLifeService::CalendarDelete,
                        "{\"eventId\":\"" + block_id + "\",\"confirmationToken\":\"" + token + "\"}");
    ExpectOk(deleted);
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                  "{\"undoOperationId\":\"" + String(deleted.value, "undoOperationId") + "\"}"));
    int restored_reminders = 0;
    for (const auto& reminder : fixture.storage.state_copy.reminders) {
        if (reminder.event_id == block_id) ++restored_reminders;
    }
    CHECK(restored_reminders == 2);
}

void TestCommitFailureRollsBack() {
    Fixture fixture;
    fixture.storage.fail_next_save = true;
    Json failed = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                       "{\"title\":\"不能半写\",\"startsAt\":\"" + fixture.At(3600) + "\"}");
    ExpectError(failed);
    CHECK(String(failed.value, "reason") == "storage_commit_failed");
    Json query = Call(fixture.service, &VoiceLifeService::CalendarFind,
                      "{\"query\":\"不能半写\"}");
    ExpectOk(query);
    CHECK(Number(query.value, "total") == 0);
}

void TestBlockWeakReminder() {
    Fixture fixture;
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"课程\",\"startsAt\":\"" + fixture.At(3600) +
                            "\",\"kind\":\"time_block\",\"durationMinutes\":60}");
    ExpectOk(created);
    fixture.now += 3600 - 15 * 60;
    fixture.service.Tick();
    CHECK(fixture.announcements.size() == 1);
    CHECK(fixture.announcements[0].find("提前提示") != std::string::npos);
}

void TestReminderLifecycleAndSnooze() {
    Fixture fixture;
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"到期提醒\",\"startsAt\":\"" + fixture.At(1) + "\"}");
    ExpectOk(created);
    fixture.now += 2;
    fixture.service.Tick();
    CHECK(fixture.announcements.size() == 1);
    Json due = Call(fixture.service, &VoiceLifeService::ReminderListDue);
    ExpectOk(due);
    CHECK(Number(due.value, "total") == 1);
    const cJSON* due_item = ArrayItem(due.value, "reminders", 0);
    const std::string reminder_id = String(due_item, "reminderId");
    CHECK(String(due_item, "status") == "pushed");

    Json details = Call(fixture.service, &VoiceLifeService::ReminderGetDetails,
                       "{\"reminderId\":\"" + reminder_id + "\"}");
    ExpectOk(details);
    CHECK(String(cJSON_GetObjectItemCaseSensitive(details.value, "reminder"), "title") == "到期提醒");
    Json closed = Call(fixture.service, &VoiceLifeService::ReminderClose,
                       "{\"reminderId\":\"" + reminder_id + "\"}");
    ExpectOk(closed);
    Json closed_again = Call(fixture.service, &VoiceLifeService::ReminderClose,
                             "{\"reminderId\":\"" + reminder_id + "\"}");
    ExpectOk(closed_again);
    CHECK(Bool(closed_again.value, "alreadyClosed"));

    Json snooze_event = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                             "{\"title\":\"推迟测试\",\"startsAt\":\"" + fixture.At(1) + "\"}");
    ExpectOk(snooze_event);
    fixture.now += 2;
    fixture.service.Tick();
    Json snooze_due = Call(fixture.service, &VoiceLifeService::ReminderListDue);
    const std::string snooze_id = String(ArrayItem(snooze_due.value, "reminders", 0), "reminderId");
    Json first_snooze = Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                             "{\"reminderId\":\"" + snooze_id + "\",\"minutes\":1}");
    ExpectOk(first_snooze);
    CHECK(Number(first_snooze.value, "snoozeCount") == 1);
    Json duplicate_snooze = Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                                 "{\"reminderId\":\"" + snooze_id + "\",\"minutes\":1}");
    ExpectOk(duplicate_snooze);
    CHECK(Bool(duplicate_snooze.value, "alreadySnoozed"));

    for (int expected_count = 2; expected_count <= 3; ++expected_count) {
        fixture.now = voicelife::ParseIso8601(String(first_snooze.value, "nextTriggerAt")) + 1;
        first_snooze = Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                            "{\"reminderId\":\"" + snooze_id + "\",\"minutes\":1}");
        // The first call above is intentionally made while the reminder is
        // snoozed; it must be idempotent. Tick then makes it pushable again.
        CHECK(Bool(first_snooze.value, "alreadySnoozed"));
        fixture.service.Tick();
        first_snooze = Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                            "{\"reminderId\":\"" + snooze_id + "\",\"minutes\":1}");
        ExpectOk(first_snooze);
        CHECK(Number(first_snooze.value, "snoozeCount") == expected_count);
    }
    fixture.now = voicelife::ParseIso8601(String(first_snooze.value, "nextTriggerAt")) + 1;
    fixture.service.Tick();
    Json fourth = Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                       "{\"reminderId\":\"" + snooze_id + "\",\"minutes\":1}");
    ExpectError(fourth);
}

void TestReminderSnoozeBoundariesAndClosedState() {
    Fixture fixture;
    Json future = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                       "{\"title\":\"边界提醒\",\"startsAt\":\"" + fixture.At(60) + "\"}");
    ExpectOk(future);
    const std::string future_event_id = String(future.value, "eventId");
    const auto future_reminder = std::find_if(
        fixture.storage.state_copy.reminders.begin(), fixture.storage.state_copy.reminders.end(),
        [&future_event_id](const auto& reminder) { return reminder.event_id == future_event_id && !reminder.weak; });
    CHECK(future_reminder != fixture.storage.state_copy.reminders.end());
    const std::string reminder_id = future_reminder->id;
    ExpectError(Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                     "{\"reminderId\":\"" + reminder_id + "\",\"minutes\":1}"));

    fixture.now += 61;
    fixture.service.Tick();
    for (const char* minutes : {"0", "1441", "1.5"}) {
        ExpectError(Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                         "{\"reminderId\":\"" + reminder_id + "\",\"minutes\":" + minutes + "}"));
    }
    Json max = Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                    "{\"reminderId\":\"" + reminder_id + "\",\"minutes\":1440}");
    ExpectOk(max);
    CHECK(Number(max.value, "snoozeCount") == 1);

    Json closable = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         "{\"title\":\"关闭后不可推迟\",\"startsAt\":\"" + fixture.At(1) + "\"}");
    ExpectOk(closable);
    const std::string close_event_id = String(closable.value, "eventId");
    fixture.now += 2;
    fixture.service.Tick();
    const auto close_reminder = std::find_if(
        fixture.storage.state_copy.reminders.begin(), fixture.storage.state_copy.reminders.end(),
        [&close_event_id](const auto& reminder) { return reminder.event_id == close_event_id && !reminder.weak; });
    CHECK(close_reminder != fixture.storage.state_copy.reminders.end());
    const std::string close_reminder_id = close_reminder->id;
    ExpectOk(Call(fixture.service, &VoiceLifeService::ReminderClose,
                  "{\"reminderId\":\"" + close_reminder_id + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::ReminderSnooze,
                     "{\"reminderId\":\"" + close_reminder_id + "\",\"minutes\":1}"));
}

void TestNotesAndExpiry() {
    Fixture fixture;
    Json note = Call(fixture.service, &VoiceLifeService::NoteRecord,
                     "{\"content\":\"车停在 B2-17\",\"category\":\"停车\"}");
    ExpectOk(note);
    Json notes = Call(fixture.service, &VoiceLifeService::NoteQuery,
                      "{\"query\":\"B2\"}");
    ExpectOk(notes);
    CHECK(Number(notes.value, "total") == 1);
    for (const char* sensitive : {"WiFi 密码是 abc", "验证码 123456", "api key: secret", "token=abc"}) {
        ExpectError(Call(fixture.service, &VoiceLifeService::NoteRecord,
                         std::string("{\"content\":\"") + sensitive + "\"}"));
    }
    fixture.now += 24 * 60 * 60;
    Json expired = Call(fixture.service, &VoiceLifeService::NoteQuery);
    ExpectOk(expired);
    CHECK(Number(expired.value, "total") == 0);
}

void TestUndoAndPersistence() {
    Fixture fixture;
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"撤销创建\",\"startsAt\":\"" + fixture.At(3600) + "\"}");
    const std::string create_undo = String(created.value, "undoOperationId");
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                  "{\"undoOperationId\":\"" + create_undo + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                     "{\"undoOperationId\":\"" + create_undo + "\"}"));

    Json editable = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                         "{\"title\":\"原标题\",\"startsAt\":\"" + fixture.At(7200) + "\"}");
    const std::string edit_id = String(editable.value, "eventId");
    Json modified = Call(fixture.service, &VoiceLifeService::CalendarModify,
                         "{\"eventId\":\"" + edit_id + "\",\"title\":\"新标题\"}");
    ExpectOk(modified);
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                  "{\"undoOperationId\":\"" + String(modified.value, "undoOperationId") + "\"}"));
    Json old_title = Call(fixture.service, &VoiceLifeService::CalendarFind,
                          "{\"query\":\"原标题\"}");
    ExpectOk(old_title);
    CHECK(Number(old_title.value, "total") == 1);

    Json persisted = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                          "{\"title\":\"重启后仍在\",\"startsAt\":\"" + fixture.At(10800) + "\"}");
    CHECK(fixture.storage.save_count > 0);
    VoiceLifeService restored(&fixture.storage, [&fixture]() { return fixture.now; });
    CHECK(restored.Initialize());
    Json restored_query = Call(restored, &VoiceLifeService::CalendarFind,
                               "{\"query\":\"重启后仍在\"}");
    ExpectOk(restored_query);
    CHECK(Number(restored_query.value, "total") == 1);
    CHECK(String(persisted.value, "eventId") == String(ArrayItem(restored_query.value, "candidates", 0), "eventId"));
}

void TestUndoExpiryAndInvalidConfirmationTokens() {
    Fixture fixture;
    Json created = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                        "{\"title\":\"令牌测试\",\"startsAt\":\"" + fixture.At(3600) + "\"}");
    ExpectOk(created);
    const std::string event_id = String(created.value, "eventId");
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarDelete,
                     "{\"eventId\":\"" + event_id + "\",\"confirmationToken\":\"delete-wrong\"}"));

    Json modified = Call(fixture.service, &VoiceLifeService::CalendarModify,
                         "{\"eventId\":\"" + event_id + "\",\"title\":\"十分钟后不能撤销\"}");
    ExpectOk(modified);
    const std::string undo_id = String(modified.value, "undoOperationId");
    fixture.now += 10 * 60 + 1;
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                     "{\"undoOperationId\":\"" + undo_id + "\"}"));
    ExpectError(Call(fixture.service, &VoiceLifeService::CalendarUndo,
                     R"({"undoOperationId":"missing"})"));
}

void TestInvalidJournalStartsFromEmptyState() {
    int64_t now = Fixture::kInitialNow;
    MemoryStorage storage;
    storage.fail_next_load = true;
    VoiceLifeService service(&storage, [&now]() { return now; });
    CHECK(service.Initialize());
    CHECK(storage.save_count == 1);
    CHECK(storage.has_state);
    CHECK(storage.state_copy.events.empty());
    ExpectOk(Call(service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"恢复后可写\",\"startsAt\":\"" + voicelife::FormatUtc(now + 60) + "\"}"));
}

void TestRetryAndBatching() {
    Fixture fixture;
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"批量一\",\"startsAt\":\"" + fixture.At(1) + "\"}"));
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarCreate,
                  "{\"title\":\"批量二\",\"startsAt\":\"" + fixture.At(2) + "\"}"));
    fixture.now += 3;
    fixture.speech_succeeds = false;
    fixture.service.Tick();
    CHECK(fixture.announcements.size() == 1);
    Json still_due = Call(fixture.service, &VoiceLifeService::ReminderListDue);
    CHECK(Number(still_due.value, "total") == 2);

    fixture.speech_succeeds = true;
    fixture.service.Tick();
    CHECK(fixture.announcements.size() == 2);
    CHECK(fixture.announcements.back().find("批量一") != std::string::npos);
    CHECK(fixture.announcements.back().find("批量二") != std::string::npos);
    fixture.service.Tick();
    CHECK(fixture.announcements.size() == 2);
}

void TestPauseResumeTerminate() {
    Fixture fixture;
    Json recurring = Call(fixture.service, &VoiceLifeService::CalendarCreate,
                           "{\"title\":\"每日站会\",\"startsAt\":\"" + fixture.At(3600) +
                               "\",\"recurrence\":{\"frequency\":\"daily\"}}");
    ExpectOk(recurring);
    const std::string event_id = String(recurring.value, "eventId");
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarPauseSeries,
                  "{\"eventId\":\"" + event_id + "\"}"));
    Json paused_again = Call(fixture.service, &VoiceLifeService::CalendarPauseSeries,
                             "{\"eventId\":\"" + event_id + "\"}");
    ExpectOk(paused_again);
    CHECK(Bool(paused_again.value, "alreadyPaused"));
    Json paused = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                       "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" + fixture.At(9000) + "\"}");
    CHECK(Number(paused.value, "total") == 0);
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarResumeSeries,
                  "{\"eventId\":\"" + event_id + "\"}"));
    Json resumed_again = Call(fixture.service, &VoiceLifeService::CalendarResumeSeries,
                              "{\"eventId\":\"" + event_id + "\"}");
    ExpectOk(resumed_again);
    CHECK(Bool(resumed_again.value, "alreadyResumed"));
    Json resumed = Call(fixture.service, &VoiceLifeService::CalendarQuery,
                        "{\"rangeStart\":\"" + fixture.At(0) + "\",\"rangeEnd\":\"" + fixture.At(9000) + "\"}");
    CHECK(Number(resumed.value, "total") == 1);

    Json needs_confirmation = Call(fixture.service, &VoiceLifeService::CalendarTerminateSeries,
                                   "{\"eventId\":\"" + event_id + "\"}");
    ExpectError(needs_confirmation);
    const std::string confirmation_token = String(needs_confirmation.value, "confirmationToken");
    ExpectOk(Call(fixture.service, &VoiceLifeService::CalendarTerminateSeries,
                  "{\"eventId\":\"" + event_id + "\",\"confirmationToken\":\"" +
                      confirmation_token + "\"}"));
    Json terminated_again = Call(fixture.service, &VoiceLifeService::CalendarTerminateSeries,
                                 "{\"eventId\":\"" + event_id + "\",\"confirmationToken\":\"" +
                                     confirmation_token + "\"}");
    ExpectOk(terminated_again);
    CHECK(Bool(terminated_again.value, "alreadyTerminated"));
    Json terminated = Call(fixture.service, &VoiceLifeService::CalendarFind,
                           "{\"query\":\"每日站会\"}");
    CHECK(Number(terminated.value, "total") == 0);
}

void Run(const char* name, const std::function<void()>& test) {
    try {
        test();
        std::cout << "PASS " << name << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << name << ": " << error.what() << "\n";
        throw;
    }
}

}  // namespace

int main() {
    Run("ISO-8601 parsing and formatting", TestIso8601);
    Run("concise spoken output and bounded query payload", TestConciseSpokenOutputAndQueryLimit);
    Run("calendar query speech boundaries", TestCalendarQuerySpeechBoundaries);
    Run("exact duplicate create is idempotent", TestExactDuplicateCreateIsIdempotent);
    Run("query boundaries, overlapping blocks and find validation",
        TestQueryBoundariesAndFindValidation);
    Run("calendar find payload is compact and bounded", TestCalendarFindPayloadIsBounded);
    Run("leap day and month-end recurrence", TestLeapDayAndMonthEndRecurrence);
    Run("calendar CRUD, query, find, delete and undo", TestCalendarCrudAndQuery);
    Run("calendar validation and conflict confirmation", TestCalendarValidationAndConflicts);
    Run("conflict tokens bind create and modify payloads",
        TestConflictTokenBindingForCreateAndModify);
    Run("#62 create contract validation", TestCreateContractValidation);
    Run("daily, weekly and monthly recurrence with repeated delivery",
        TestRecurringOccurrencesAndDelivery);
    Run("skip and undo one recurring occurrence", TestSkipOneRecurringOccurrence);
    Run("single event skip cancels immediately and is idempotent",
        TestSingleEventSkipCancelsImmediatelyAndIsIdempotent);
    Run("single event scope and ASR-tolerant exact-minute find",
        TestSingleEventScopeAndAsrTolerantFind);
    Run("weekly create aligns its first occurrence locally",
        TestWeeklyCreateAlignsFirstOccurrenceLocally);
    Run("reminder offset and weak-reminder undo", TestReminderOffsetAndWeakReminderUndo);
    Run("storage commit rollback", TestCommitFailureRollsBack);
    Run("time block and weak reminder", TestBlockWeakReminder);
    Run("reminder close, details and three-snooze lifecycle", TestReminderLifecycleAndSnooze);
    Run("reminder snooze bounds and closed state", TestReminderSnoozeBoundariesAndClosedState);
    Run("notes, sensitive-content rejection and expiry", TestNotesAndExpiry);
    Run("undo snapshots and restart persistence", TestUndoAndPersistence);
    Run("undo expiry and invalid confirmation tokens", TestUndoExpiryAndInvalidConfirmationTokens);
    Run("invalid journal recovery starts from empty state", TestInvalidJournalStartsFromEmptyState);
    Run("delivery retry and batching", TestRetryAndBatching);
    Run("pause, resume and terminate series", TestPauseResumeTerminate);
    return 0;
}
