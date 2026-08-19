// 日程提醒 → IM 强提醒桥的主机测试（TDD 先写）。
// 复用 schedule_reminder_service_test.cc 的 ScriptedFixture 与 im_runtime/
// action_channel 的测试支持：真 ImRuntime 驱动至 ready，验证通知载荷、
// 动作窗口、ack/snooze 执行与断线重连语义。

#define main ExistingScheduleReminderTestMain
#include "schedule_reminder_service_test.cc"
#undef main

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "im_action_channel_test_support.h"
#include "im_runtime_test_support.h"
#include "voicelife/contracts/im/notification_intent.h"
#include "voicelife/im/im_action_channel.h"
#include "voicelife/reminder_im/im_reminder_bridge.h"

namespace {

using voicelife::contracts::im::NotificationIntent;
using voicelife::contracts::im::ParseNotificationIntent;
using voicelife::contracts::im::ParseReminderActionResult;
using voicelife::contracts::im::ReminderActionCommand;
using voicelife::contracts::im::ReminderActionResult;
using voicelife::im::ActionRunStatus;
using voicelife::im::ActionWindow;
using voicelife::im::ImActionCommandStream;
using voicelife::im::ImHttpRequest;
using voicelife::im::ImHttpResponse;
using voicelife::im::ImRuntime;
using voicelife::im::ImTransport;
using voicelife::im::ImTransportStatus;
using voicelife::im::StreamRead;
using voicelife::im::StreamReadStatus;
using voicelife::reminder_im::ImReminderBridge;
using voicelife::test::action_channel::FakeClock;
using voicelife::test::action_channel::FakeStream;
using voicelife::test::im_runtime_support::FakeConfig;
using voicelife::test::im_runtime_support::FakeCredentials;
using voicelife::test::im_runtime_support::FakeReadiness;

/// 网关受理响应的共享 fixture：带 actionStream 的强提醒窗口。
constexpr const char* kSubmissionBody =
    "{\"businessEventId\":\"event-1\",\"status\":\"accepted\",\"deliveries\":[],"
    "\"actionStream\":{\"reminderTriggerId\":\"reminder-1-77\",\"expiresAt\":\"2026-08-03T00:10:00.000Z\"}}";

/// 记录全部请求并可控响应的假传输：探针 GET 恒返回已认证 404。
class RecordingTransport final : public ImTransport {
   public:
    ImHttpResponse next_post_response{};

    ImHttpResponse Post(const ImHttpRequest& request) override {
        requests.push_back(request);
        return next_post_response;
    }
    ImHttpResponse Get(const ImHttpRequest& request) override {
        requests.push_back(request);
        return {.status = ImTransportStatus::kHttpError, .status_code = 404, .body = "Not Found", .message = "404"};
    }

    std::vector<ImHttpRequest> requests;
};

/// 装配真 ImRuntime 至 ready，并持有传输记录。
struct BridgeRuntime {
    FakeConfig config;
    FakeCredentials credentials;
    FakeReadiness readiness;
    RecordingTransport* transport = nullptr;
    ImRuntime runtime{config, credentials, readiness, [this](const std::string&) {
                          auto created = std::make_unique<RecordingTransport>();
                          transport = created.get();
                          return created;
                      }};

    /// 启动并认证探针，使 Runtime 进入 ready。Transport 由 Start 创建，响应须在其后配置。
    bool MakeReady(const std::string& submission_body = kSubmissionBody) {
        if (!runtime.Start().ok()) return false;
        transport->next_post_response = {.status = ImTransportStatus::kSuccess,
                                         .status_code = 200,
                                         .body = submission_body,
                                         .message = "ok"};
        return runtime.ProbeGateway().status_code == 404;
    }
};

/// 构造归属当前设备与指定触发标识的动作命令。
ReminderActionCommand MakeBridgeCommand(const std::string& trigger, const std::string& action) {
    ReminderActionCommand command;
    command.schemaVersion = "1";
    command.commandId = "command-1";
    command.operationId = "operation-1";
    command.correlationId = "correlation-1";
    command.deviceId = "device-test";
    command.actorBindingId = "binding-1";
    command.reminderTriggerId = trigger;
    command.action = action;
    command.minutes = action == "snooze" ? std::optional<int>(10) : std::nullopt;
    command.occurredAt = "2026-08-03T00:00:00.000Z";
    command.expiresAt = "2026-08-03T00:05:00.000Z";
    return command;
}

/// 构造一次提醒触发事件快照。
ReminderFireNotice MakeNotice(int64_t task_id) {
    ReminderFireNotice notice;
    notice.schedule_id = 1;
    notice.task_id = task_id;
    notice.event = "会议提醒";
    notice.trigger_time = At(1'100);
    return notice;
}

/// 流的生命周期记录：测试侧持有，流实例被桥释放后仍可断言。
struct StreamLog {
    std::vector<std::string> open_cursors;
};

/// 把 Open 游标记录到外部日志的假动作流：桥按轮次释放流实例，但断言
/// 目标（游标、命令）全部落在测试持有的日志上，不存在悬垂读取。
class LoggedStream final : public ImActionCommandStream {
   public:
    StreamLog& log;
    std::vector<ReminderActionCommand> commands;
    StreamReadStatus terminal = StreamReadStatus::kEndOfStream;

    explicit LoggedStream(StreamLog& target) : log(target) {}

    bool Open(const std::string& last_event_id) override {
        log.open_cursors.push_back(last_event_id);
        return true;
    }
    StreamRead Next() override {
        if (commands.empty()) return {terminal, {}};
        ReminderActionCommand command = commands.front();
        commands.erase(commands.begin());
        return {StreamReadStatus::kCommand, command};
    }
    void Close() override {}
};

/// 从回传请求体解析动作结果。
ReminderActionResult ParseResultBody(const ImHttpRequest& request) {
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(request.body, root).ok(), "回传请求体必须是合法 JSON");
    ReminderActionResult parsed;
    Check(ParseReminderActionResult(root, parsed).ok(), "回传请求体必须通过契约校验");
    return parsed;
}

void CheckSkipsWhenRuntimeNotReady() {
    BridgeRuntime runtime;  // 未 Start
    FakeClock clock;
    int factory_calls = 0;
    ImReminderBridge bridge(runtime.runtime, runtime.credentials, clock, nullptr,
                            [&factory_calls](const std::string&) {
                                ++factory_calls;
                                return std::unique_ptr<ImActionCommandStream>(nullptr);
                            });
    bridge.NotifyReminderFired(MakeNotice(7));
    bridge.RequestStop();
    bridge.RunWorkerLoop();
    Check(factory_calls == 0 && runtime.transport == nullptr, "IM 未 ready 不得提交通知或建立动作流");
}

void CheckSkipsWithoutUserId() {
    BridgeRuntime runtime;
    runtime.config.result = Result<voicelife::im::ImRuntimeConfig>::Success(
        {.enabled = true, .gateway_origin = "https://gateway.example", .user_id = std::nullopt});
    Check(runtime.MakeReady(), "运行时应就绪");
    FakeClock clock;
    int factory_calls = 0;
    ImReminderBridge bridge(runtime.runtime, runtime.credentials, clock, nullptr,
                            [&factory_calls](const std::string&) {
                                ++factory_calls;
                                return std::unique_ptr<ImActionCommandStream>(nullptr);
                            });
    bridge.NotifyReminderFired(MakeNotice(7));
    bridge.RequestStop();
    bridge.RunWorkerLoop();
    Check(factory_calls == 0 && runtime.transport->requests.size() == 1, "无 userId 不得提交通知");
}

void CheckSkipsWhenSubmissionFails() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady(), "运行时应就绪");
    runtime.transport->next_post_response = {.status = ImTransportStatus::kNetworkFailure,
                                             .status_code = 0,
                                             .body = {},
                                             .message = "net"};  // 覆盖默认成功响应
    FakeClock clock;
    int factory_calls = 0;
    ImReminderBridge bridge(runtime.runtime, runtime.credentials, clock, nullptr,
                            [&factory_calls](const std::string&) {
                                ++factory_calls;
                                return std::unique_ptr<ImActionCommandStream>(nullptr);
                            });
    bridge.NotifyReminderFired(MakeNotice(7));
    bridge.RequestStop();
    bridge.RunWorkerLoop();
    Check(factory_calls == 0 && runtime.transport->requests.size() == 2, "提交失败不得建立动作流");
}

void CheckSkipsWithoutActionStream() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady("{\"businessEventId\":\"e\",\"status\":\"accepted\",\"deliveries\":[]}"), "运行时应就绪");
    FakeClock clock;
    int factory_calls = 0;
    ImReminderBridge bridge(runtime.runtime, runtime.credentials, clock, nullptr,
                            [&factory_calls](const std::string&) {
                                ++factory_calls;
                                return std::unique_ptr<ImActionCommandStream>(nullptr);
                            });
    bridge.NotifyReminderFired(MakeNotice(7));
    bridge.RequestStop();
    bridge.RunWorkerLoop();
    Check(factory_calls == 0, "受理结果无 actionStream 不得建立动作流");
}

void CheckAcknowledgeExecutesAndReports() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady(), "运行时应就绪");
    ScriptedFixture fixture({MakeSchedule(1, "会议提醒", At(1'100))});
    Check(fixture.reminder.Start().ok(), "提醒服务应启动");

    FakeClock clock;
    std::vector<FakeStream*> streams;
    int factory_calls = 0;
    std::string last_trigger;
    ImReminderBridge bridge(
        runtime.runtime, runtime.credentials, clock, &fixture.reminder,
        [&streams, &factory_calls, &last_trigger](const std::string& trigger) {
            ++factory_calls;
            last_trigger = trigger;
            auto stream = std::make_unique<FakeStream>();
            stream->commands.push_back(MakeBridgeCommand(trigger, "acknowledge"));
            streams.push_back(stream.get());
            return stream;
        });
    bridge.NotifyReminderFired(MakeNotice(77));
    bridge.RequestStop();
    bridge.RunWorkerLoop();

    Check(factory_calls == 1 && streams.size() == 1, "强提醒应建立一次动作流");
    Check(last_trigger == "reminder-1-77", "动作流必须绑定提醒触发标识");

    // 通知载荷契约与字段。
    voicelife::JsonValue root;
    Check(voicelife::ParseJson(runtime.transport->requests[1].body, root).ok(), "通知请求体必须是合法 JSON");
    NotificationIntent intent;
    Check(ParseNotificationIntent(root, intent).ok(), "通知请求体必须通过契约校验");
    Check(intent.kind == "reminder_due" && intent.reminderType == "strong", "强提醒语义必须正确");
    Check(intent.businessEventId == intent.correlationId && intent.correlationId == intent.reminderTriggerId &&
              intent.reminderTriggerId == "reminder-1-77",
          "业务事件、关联与触发标识必须一致");
    Check(intent.recipient.deviceId == "device-test" && intent.recipient.userId == "user-test",
          "收件人必须使用运行时身份");
    Check(intent.scheduleId == "1" && intent.taskId == "77" && intent.instanceId == "1",
          "日程、任务与实例标识必须来自触发快照");
    Check(intent.actions.size() == 2 && intent.actions[0].type == "acknowledge" &&
              intent.actions[0].label == "知道了" && intent.actions[1].type == "snooze" &&
              intent.actions[1].label == "推迟 10 分钟" && intent.actions[1].minutes == 10,
          "按钮必须包含知道了与推迟十分钟");
    Check(intent.plannedAt == intent.triggerAt && intent.plannedAt == "1970-01-01T00:18:20.000Z",
          "计划与触发时间必须等于提醒时刻");
    Check(intent.occurredAt == clock.now, "发生时间必须来自时钟");

    // 回传载荷：acknowledge 成功且不携带下次触发时间。
    Check(runtime.transport->requests.size() == 3, "探针、通知与结果应各一次");
    const ReminderActionResult result = ParseResultBody(runtime.transport->requests[2]);
    Check(result.status == "succeeded" && !result.nextTriggerAt.has_value(),
          "acknowledge 成功且不得携带下次触发时间");
    Check(result.operationId == "operation-1" && result.reminderTriggerId == "reminder-1-77",
          "回传必须绑定操作与触发标识");
    Check(result.occurredAt == clock.now, "回传时间必须来自时钟");
}

void CheckSnoozeExecutesAndPersists() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady(), "运行时应就绪");
    ScriptedFixture fixture({MakeSchedule(1, "会议提醒", At(1'100))});
    Check(fixture.reminder.Start().ok(), "提醒服务应启动");

    FakeClock clock;
    std::vector<FakeStream*> streams;
    ImReminderBridge bridge(
        runtime.runtime, runtime.credentials, clock, &fixture.reminder,
        [&streams](const std::string& trigger) {
            auto stream = std::make_unique<FakeStream>();
            stream->commands.push_back(MakeBridgeCommand(trigger, "snooze"));
            streams.push_back(stream.get());
            return stream;
        });
    bridge.NotifyReminderFired(MakeNotice(77));
    bridge.RequestStop();
    bridge.RunWorkerLoop();

    // 推迟状态持久化：次数递增、重复任务与触发时间写入仓储。
    const auto stored = fixture.repository.FindById(1);
    Check(stored.ok() && stored.value->snooze_count == 1 && stored.value->repeat_task_id.has_value() &&
              stored.value->repeat_trigger_at == At(1'600),
          "推迟应持久化次数、重复任务标识与十分钟后的触发时间");
    Check(fixture.speech.texts.empty(), "推迟本身不应触发语音播报");

    // 回传载荷：succeeded + 下一次触发时间。
    const ReminderActionResult result = ParseResultBody(runtime.transport->requests[2]);
    Check(result.status == "succeeded" && result.nextTriggerAt == "1970-01-01T00:26:40.000Z",
          "snooze 成功必须携带十分钟后的下次触发时间");
}

void CheckFourthSnoozeRejectedWithLimit() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady(), "运行时应就绪");
    // 预置已接受 3 次推迟的日程。用 kCompleted 而非 kActive：Start 的同步轮会
    // 重置 kActive 的推迟状态，而真实场景推迟次数正是在完成后的重复提醒中累加的。
    ScriptedFixture fixture({MakeSchedule(1, "会议提醒", At(1'100), std::nullopt, std::nullopt,
                                          ScheduleStatus::kCompleted, 3)});
    Check(fixture.reminder.Start().ok(), "提醒服务应启动");

    FakeClock clock;
    std::vector<FakeStream*> streams;
    ImReminderBridge bridge(
        runtime.runtime, runtime.credentials, clock, &fixture.reminder,
        [&streams](const std::string& trigger) {
            auto stream = std::make_unique<FakeStream>();
            stream->commands.push_back(MakeBridgeCommand(trigger, "snooze"));
            streams.push_back(stream.get());
            return stream;
        });
    bridge.NotifyReminderFired(MakeNotice(77));
    bridge.RequestStop();
    bridge.RunWorkerLoop();

    const ReminderActionResult result = ParseResultBody(runtime.transport->requests[2]);
    Check(result.status == "failed" && result.errorCode == "snooze_limit",
          "第四次推迟必须回传 failed 与 snooze_limit");
    const auto stored = fixture.repository.FindById(1);
    Check(stored.ok() && stored.value->snooze_count == 3, "拒绝不得修改已持久化的推迟次数");
}

void CheckReconnectReusesChannelAndCursor() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady(), "运行时应就绪");
    ScriptedFixture fixture({MakeSchedule(1, "会议提醒", At(1'100))});
    Check(fixture.reminder.Start().ok(), "提醒服务应启动");

    FakeClock clock;
    std::vector<std::unique_ptr<StreamLog>> logs;
    std::atomic<int> factory_calls{0};
    ImReminderBridge bridge(
        runtime.runtime, runtime.credentials, clock, &fixture.reminder,
        [&logs, &factory_calls](const std::string& trigger) {
            // 日志对象先独立落地，再让流引用它：桥释放流不影响日志的存活。
            auto log = std::make_unique<StreamLog>();
            StreamLog* log_ptr = log.get();
            logs.push_back(std::move(log));
            auto stream = std::make_unique<LoggedStream>(*log_ptr);
            if (factory_calls.load() == 0) {
                stream->commands.push_back(MakeBridgeCommand(trigger, "snooze"));
                stream->terminal = StreamReadStatus::kNetworkError;
            } else {
                stream->terminal = StreamReadStatus::kEndOfStream;
            }
            factory_calls.fetch_add(1);
            return stream;
        },
        std::chrono::milliseconds(1));
    bridge.NotifyReminderFired(MakeNotice(77));

    // 同步模式下停止请求会中断退避睡眠，重连无从发生；改为线程驱动，
    // 等重连完成后再请求停止以解阻塞等待。
    std::thread worker([&bridge] { bridge.RunWorkerLoop(); });
    for (int i = 0; i < 2000 && factory_calls.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bridge.RequestStop();
    worker.join();

    Check(factory_calls.load() == 2 && logs.size() == 2, "断线后应重连一次");
    Check(logs[0]->open_cursors.size() == 1 && logs[0]->open_cursors.front().empty(),
          "首次连接不得携带游标");
    Check(logs[1]->open_cursors.size() == 1 && logs[1]->open_cursors.front() == "command-1",
          "重连必须携带上次确认的游标");
    Check(runtime.transport->requests.size() == 3, "探针、通知与结果应各一次");
    Check(runtime.transport->requests[2].path == "/v1/devices/device-test/reminder-actions/command-1/result",
          "结果必须回传到对应命令");
    const auto stored = fixture.repository.FindById(1);
    Check(stored.ok() && stored.value->snooze_count == 1, "重复连接不得重复执行命令");
}

void CheckStopInterruptsReconnectWait() {
    BridgeRuntime runtime;
    Check(runtime.MakeReady(), "运行时应就绪");
    FakeClock clock;
    std::atomic<int> factory_calls{0};
    ImReminderBridge bridge(
        runtime.runtime, runtime.credentials, clock, nullptr,
        [&factory_calls](const std::string&) {
            factory_calls.fetch_add(1);
            auto stream = std::make_unique<FakeStream>();
            stream->terminal = StreamReadStatus::kNetworkError;  // 持续断线 → 退避循环
            return stream;
        },
        std::chrono::milliseconds(500));
    bridge.NotifyReminderFired(MakeNotice(7));

    std::thread worker([&bridge] { bridge.RunWorkerLoop(); });
    for (int i = 0; i < 1000 && factory_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(factory_calls.load() == 1, "worker 应已进入动作窗口并开始退避");
    const auto started = std::chrono::steady_clock::now();
    bridge.RequestStop();
    worker.join();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Check(elapsed_ms < 500, "停止请求必须中断退避睡眠，使停止延迟有界");
    Check(bridge.IsStopRequested(), "停止标志必须置位");
}

void CheckQueueOverflowDropsSilently() {
    BridgeRuntime runtime;
    FakeClock clock;
    ImReminderBridge bridge(runtime.runtime, runtime.credentials, clock, nullptr,
                            [](const std::string&) { return std::unique_ptr<ImActionCommandStream>(nullptr); });
    for (int i = 0; i < 6; ++i) {
        bridge.NotifyReminderFired(MakeNotice(i + 1));
    }
    Check(bridge.dropped_fires() == 2, "超过队列容量的触发事件应被静默丢弃并计数");
    bridge.RequestStop();
    bridge.RunWorkerLoop();
    Check(bridge.dropped_fires() == 2, "丢弃计数不应因消费而减少");
}

void CheckUnknownTriggerFails() {
    BridgeRuntime runtime;
    FakeClock clock;
    ImReminderBridge bridge(runtime.runtime, runtime.credentials, clock, nullptr,
                            [](const std::string&) { return std::unique_ptr<ImActionCommandStream>(nullptr); });
    const ReminderActionResult result = bridge.Execute(MakeBridgeCommand("reminder-9-1", "snooze"));
    Check(result.status == "failed" && result.errorCode == "unknown_window",
          "窗口外命令必须回传 failed 与 unknown_window");
    Check(result.operationId == "operation-1" && result.reminderTriggerId == "reminder-9-1",
          "失败回传必须保留操作与触发标识");
}

}  // namespace

/** @brief 执行日程提醒 IM 桥测试。 @return 全部断言通过时返回 0。 */
int main() {
    CheckSkipsWhenRuntimeNotReady();
    CheckSkipsWithoutUserId();
    CheckSkipsWhenSubmissionFails();
    CheckSkipsWithoutActionStream();
    CheckAcknowledgeExecutesAndReports();
    CheckSnoozeExecutesAndPersists();
    CheckFourthSnoozeRejectedWithLimit();
    CheckReconnectReusesChannelAndCursor();
    CheckStopInterruptsReconnectWait();
    CheckQueueOverflowDropsSilently();
    CheckUnknownTriggerFails();
    std::cout << "PASS im_reminder_bridge_test\n";
    return EXIT_SUCCESS;
}
