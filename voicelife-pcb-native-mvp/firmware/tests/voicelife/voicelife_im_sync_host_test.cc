#include "voicelife_im_sync.h"
#include "voicelife_service.h"

#include <cJSON.h>

#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voicelife {

FlashStorage::~FlashStorage() = default;
bool FlashStorage::Initialize() { return false; }
bool FlashStorage::Load(State*) { return false; }
bool FlashStorage::Save(const State&) { return false; }

}  // namespace voicelife

namespace {

using voicelife::ImHttpResponse;
using voicelife::ImTransport;
using voicelife::State;
using voicelife::Storage;
using voicelife::VoiceLifeImSync;
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
        *state = has_state ? state_copy : State{};
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
        return true;
    }

    bool initialized = false;
    bool has_state = false;
    bool fail_next_save = false;
    State state_copy;
};

struct RecordedRequest {
    std::string method;
    std::string path;
    std::string body;
};

class FakeTransport final : public ImTransport {
public:
    ImHttpResponse Request(const std::string& method, const std::string& path,
                           const std::string& body) override {
        requests.push_back({method, path, body});
        if (responses.empty()) return {false, 0, {}, "no fake response"};
        ImHttpResponse response = std::move(responses.front());
        responses.pop_front();
        return response;
    }

    void Json(int status, const std::string& body) {
        responses.push_back({true, status, body, {}});
    }
    void NetworkFailure(const std::string& error = "offline") {
        responses.push_back({false, 0, {}, error});
    }

    std::deque<ImHttpResponse> responses;
    std::vector<RecordedRequest> requests;
};

struct Json final {
    cJSON* value = nullptr;
    explicit Json(cJSON* input) : value(input) {}
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

std::string String(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsString(value) && value->valuestring != nullptr);
    return value->valuestring;
}

cJSON* Parse(const std::string& text) {
    cJSON* value = cJSON_Parse(text.c_str());
    CHECK(value != nullptr);
    return value;
}

VoiceLifeImSync::Config SyncConfig() {
    VoiceLifeImSync::Config config;
    config.device_id = "98:a3:16:e6:91:dc";
    config.poll_seconds = 5;
    return config;
}

struct Fixture final {
    static constexpr int64_t kInitialNow = 1785542400;

    Fixture()
        : service(&storage, [this]() { return now; }),
          sync(&service, &transport, SyncConfig()) {
        CHECK(service.Initialize());
    }

    std::string At(int64_t delta) const { return voicelife::FormatUtc(now + delta); }

    std::string CreateDue(const std::string& title = "喝水", int64_t delta = 1) {
        Json created = Call(service, &VoiceLifeService::CalendarCreate,
                            "{\"title\":\"" + title + "\",\"startsAt\":\"" + At(delta) + "\"}");
        CHECK(Bool(created.value, "ok"));
        const std::string event_id = String(created.value, "eventId");
        for (const auto& reminder : storage.state_copy.reminders) {
            if (reminder.event_id == event_id && !reminder.weak) return reminder.id;
        }
        throw TestFailure("created event has no reminder");
    }

    int64_t now = kInitialNow;
    MemoryStorage storage;
    VoiceLifeService service;
    FakeTransport transport;
    VoiceLifeImSync sync;
};

void QueueNoAction(FakeTransport& transport) {
    transport.Json(200, R"({"ok":true,"action":null})");
}

void TestDueRetryAndIdempotency() {
    Fixture fixture;
    const std::string reminder_id = fixture.CreateDue();
    fixture.now += 2;
    fixture.transport.NetworkFailure();
    QueueNoAction(fixture.transport);
    CHECK(!fixture.sync.PollOnce());
    CHECK(fixture.transport.requests.size() == 2);
    CHECK(fixture.sync.ConsecutiveFailures() > 0);

    fixture.transport.Json(200, R"({"ok":true,"delivery":{"status":"sent"}})");
    QueueNoAction(fixture.transport);
    CHECK(fixture.sync.PollOnce());
    CHECK(fixture.storage.state_copy.reminders.front().im_reported_trigger_at ==
          fixture.storage.state_copy.reminders.front().trigger_at);
    cJSON* due = Parse(fixture.transport.requests[2].body);
    CHECK(String(due, "deviceId") == "98:a3:16:e6:91:dc");
    CHECK(String(due, "reminderId") == reminder_id);
    CHECK(String(due, "deliveryKey").find(reminder_id) != std::string::npos);
    cJSON_Delete(due);

    const size_t before = fixture.transport.requests.size();
    QueueNoAction(fixture.transport);
    CHECK(fixture.sync.PollOnce());
    CHECK(fixture.transport.requests.size() == before + 1);
    CHECK(fixture.transport.requests.back().method == "GET");
}

void TestGatewayRejectionRetriesDue() {
    Fixture fixture;
    fixture.CreateDue("未绑定");
    fixture.now += 2;
    fixture.transport.Json(200, R"({"ok":false,"error":"not_bound"})");
    QueueNoAction(fixture.transport);
    CHECK(!fixture.sync.PollOnce());
    CHECK(fixture.storage.state_copy.reminders.front().im_reported_trigger_at.empty());
    fixture.transport.Json(200, R"({"ok":true,"idempotent":true})");
    QueueNoAction(fixture.transport);
    CHECK(fixture.sync.PollOnce());
    CHECK(!fixture.storage.state_copy.reminders.front().im_reported_trigger_at.empty());
}

void TestCloseActionAndDuplicateReplay() {
    Fixture fixture;
    const std::string reminder_id = fixture.CreateDue("关掉我");
    fixture.now += 2;
    fixture.transport.Json(200, R"({"ok":true})");
    fixture.transport.Json(200, "{\"ok\":true,\"action\":{\"id\":\"action-close\",\"type\":\"close\",\"reminderId\":\"" + reminder_id + "\"}}");
    fixture.transport.Json(200, R"({"ok":true,"action":{"status":"acked"}})");
    CHECK(fixture.sync.PollOnce());
    CHECK(fixture.storage.state_copy.reminders.front().status == voicelife::ReminderStatus::Closed);
    CHECK(fixture.storage.state_copy.processed_im_actions.size() == 1);
    cJSON* ack = Parse(fixture.transport.requests.back().body);
    CHECK(Bool(ack, "ok"));
    CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(ack, "result")));
    cJSON_Delete(ack);

    fixture.transport.Json(200, "{\"ok\":true,\"action\":{\"id\":\"action-close\",\"type\":\"close\",\"reminderId\":\"" + reminder_id + "\"}}");
    fixture.transport.Json(200, R"({"ok":true,"idempotent":true})");
    CHECK(fixture.sync.PollOnce());
    CHECK(fixture.storage.state_copy.processed_im_actions.size() == 1);
}

void TestSnoozeBeforeLocalVoiceTick() {
    Fixture fixture;
    const std::string reminder_id = fixture.CreateDue("十分钟后");
    fixture.now += 2;
    fixture.transport.Json(200, R"({"ok":true})");
    fixture.transport.Json(200, "{\"ok\":true,\"action\":{\"id\":\"action-snooze\",\"type\":\"snooze\",\"reminderId\":\"" + reminder_id + "\",\"minutes\":10}}");
    fixture.transport.Json(200, R"({"ok":true})");
    CHECK(fixture.sync.PollOnce());
    const auto& reminder = fixture.storage.state_copy.reminders.front();
    CHECK(reminder.status == voicelife::ReminderStatus::Snoozed);
    CHECK(reminder.snooze_count == 1);
    CHECK(voicelife::ParseIso8601(reminder.trigger_at) == fixture.now + 600);
    CHECK(reminder.im_reported_trigger_at.empty());
}

void TestAckLossAndRestartIdempotency() {
    Fixture fixture;
    const std::string reminder_id = fixture.CreateDue("重启幂等");
    fixture.now += 2;
    fixture.transport.Json(200, R"({"ok":true})");
    fixture.transport.Json(200, "{\"ok\":true,\"action\":{\"id\":\"action-restart\",\"type\":\"close\",\"reminderId\":\"" + reminder_id + "\"}}");
    fixture.transport.NetworkFailure("ack lost");
    CHECK(!fixture.sync.PollOnce());
    CHECK(fixture.storage.state_copy.processed_im_actions.size() == 1);

    VoiceLifeService restarted(&fixture.storage, [&fixture]() { return fixture.now; });
    CHECK(restarted.Initialize());
    FakeTransport retry_transport;
    VoiceLifeImSync retry_sync(&restarted, &retry_transport, SyncConfig());
    retry_transport.Json(200, "{\"ok\":true,\"action\":{\"id\":\"action-restart\",\"type\":\"close\",\"reminderId\":\"" + reminder_id + "\"}}");
    retry_transport.Json(200, R"({"ok":true,"idempotent":true})");
    CHECK(retry_sync.PollOnce());
    CHECK(fixture.storage.state_copy.processed_im_actions.size() == 1);
}

void TestMalformedAndUnknownActions() {
    {
        Fixture fixture;
        fixture.transport.Json(200, R"({"ok":true,"action":{"id":"missing-fields"}})");
        CHECK(!fixture.sync.PollOnce());
        CHECK(fixture.transport.requests.size() == 1);
    }
    {
        Fixture fixture;
        const std::string reminder_id = fixture.CreateDue("未知操作");
        fixture.now += 2;
        fixture.transport.Json(200, R"({"ok":true})");
        fixture.transport.Json(200, "{\"ok\":true,\"action\":{\"id\":\"action-unknown\",\"type\":\"explode\",\"reminderId\":\"" + reminder_id + "\"}}");
        fixture.transport.Json(200, R"({"ok":false,"retryable":true})");
        CHECK(fixture.sync.PollOnce());
        cJSON* ack = Parse(fixture.transport.requests.back().body);
        CHECK(!Bool(ack, "ok"));
        cJSON_Delete(ack);
        CHECK(fixture.storage.state_copy.processed_im_actions.size() == 1);
    }
}

void TestGatewayProtocolBoundaries() {
    {
        Fixture fixture;
        fixture.transport.Json(503, R"({"ok":false,"error":"maintenance"})");
        CHECK(!fixture.sync.PollOnce());
        CHECK(fixture.sync.LastError().find("HTTP status 503") != std::string::npos);
    }
    {
        Fixture fixture;
        fixture.transport.Json(200, "not-json");
        CHECK(!fixture.sync.PollOnce());
        CHECK(fixture.sync.LastError().find("not JSON") != std::string::npos);
    }
    {
        Fixture fixture;
        fixture.transport.Json(200, "{}");
        CHECK(!fixture.sync.PollOnce());
        CHECK(fixture.sync.LastError().find("rejected action pull") != std::string::npos);
    }
    {
        Fixture fixture;
        fixture.transport.Json(200, std::string(64 * 1024 + 1, 'x'));
        CHECK(!fixture.sync.PollOnce());
        CHECK(fixture.sync.LastError().find("too large") != std::string::npos);
    }
}

void TestActionBoundsAndEncodedPaths() {
    for (int minutes : {0, 1441}) {
        Fixture fixture;
        const std::string reminder_id = fixture.CreateDue("非法推迟");
        fixture.now += 2;
        fixture.transport.Json(200, R"({"ok":true})");
        fixture.transport.Json(
            200,
            "{\"ok\":true,\"action\":{\"id\":\"action/invalid\",\"type\":\"snooze\",\"reminderId\":\"" +
                reminder_id + "\",\"minutes\":" + std::to_string(minutes) + "}}");
        fixture.transport.Json(200, R"({"ok":false,"retryable":true})");
        CHECK(fixture.sync.PollOnce());
        CHECK(fixture.transport.requests[1].path ==
              "/api/pcb/devices/98%3Aa3%3A16%3Ae6%3A91%3Adc/actions/next");
        CHECK(fixture.transport.requests[2].path == "/api/pcb/actions/action%2Finvalid/ack");
        cJSON* ack = Parse(fixture.transport.requests[2].body);
        CHECK(!Bool(ack, "ok"));
        cJSON_Delete(ack);
        CHECK(fixture.storage.state_copy.processed_im_actions.size() == 1);
        CHECK(!fixture.storage.state_copy.processed_im_actions.front().ok);
    }
}

void TestClosedReminderRejectsSnoozeAction() {
    Fixture fixture;
    const std::string reminder_id = fixture.CreateDue("已关闭");
    fixture.now += 2;
    Json closed = Call(fixture.service, &VoiceLifeService::ReminderClose,
                       "{\"reminderId\":\"" + reminder_id + "\"}");
    CHECK(Bool(closed.value, "ok"));
    fixture.transport.Json(
        200,
        "{\"ok\":true,\"action\":{\"id\":\"action-closed-snooze\",\"type\":\"snooze\",\"reminderId\":\"" +
            reminder_id + "\",\"minutes\":10}}");
    fixture.transport.Json(200, R"({"ok":false,"retryable":true})");
    CHECK(fixture.sync.PollOnce());
    cJSON* ack = Parse(fixture.transport.requests.back().body);
    CHECK(!Bool(ack, "ok"));
    CHECK(String(cJSON_GetObjectItemCaseSensitive(ack, "result"), "message").find("已经关闭") !=
          std::string::npos);
    cJSON_Delete(ack);
}

void TestMultipleDueNotifications() {
    Fixture fixture;
    const std::string first = fixture.CreateDue("第一条", 1);
    const std::string second = fixture.CreateDue("第二条", 2);
    fixture.now += 3;
    fixture.transport.Json(200, R"({"ok":true})");
    fixture.transport.Json(200, R"({"ok":true})");
    QueueNoAction(fixture.transport);
    CHECK(fixture.sync.PollOnce());
    CHECK(fixture.transport.requests.size() == 3);
    CHECK(fixture.transport.requests[0].body.find(first) != std::string::npos);
    CHECK(fixture.transport.requests[1].body.find(second) != std::string::npos);
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
    Run("due retry, persisted receipt and duplicate suppression", TestDueRetryAndIdempotency);
    Run("Gateway business rejection keeps due notification retryable", TestGatewayRejectionRetriesDue);
    Run("close action, ACK payload and duplicate replay", TestCloseActionAndDuplicateReplay);
    Run("snooze action before local voice Tick", TestSnoozeBeforeLocalVoiceTick);
    Run("lost ACK and reboot preserve action idempotency", TestAckLossAndRestartIdempotency);
    Run("malformed and unknown Gateway actions", TestMalformedAndUnknownActions);
    Run("Gateway HTTP status, JSON and response size boundaries", TestGatewayProtocolBoundaries);
    Run("action minutes bounds and URL segment encoding", TestActionBoundsAndEncodedPaths);
    Run("closed reminder rejects a later snooze action", TestClosedReminderRejectsSnoozeAction);
    Run("multiple due reminders are reported independently", TestMultipleDueNotifications);
    return 0;
}
