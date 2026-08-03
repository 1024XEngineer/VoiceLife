#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "voicelife/application/calendar_application.h"
#include "voicelife/im/im_gateway_adapter.h"
#include "voicelife/mcp/mcp_tool_gateway.h"
#include "voicelife/platform/in_memory_calendar_store.h"
#include "voicelife/platform/sequential_id_generator.h"
#include "voicelife/voice/voice_session_coordinator.h"

namespace {

using voicelife::ErrorCode;
using voicelife::Status;
using voicelife::ToolCall;
using voicelife::ToolResult;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}

class RecordingNotifications final : public voicelife::application::NotificationPort {
   public:
    Status Publish(const voicelife::application::NotificationIntent& intent) override {
        last_event_id = intent.event_id;
        ++count;
        return Status::Ok();
    }

    int count = 0;
    std::string last_event_id;
};

class RejectingNotifications final : public voicelife::application::NotificationPort {
   public:
    Status Publish(const voicelife::application::NotificationIntent&) override {
        return Status::Error(ErrorCode::kUnavailable, "通知通道不可用");
    }
};

class ConcurrentReplayStore final : public voicelife::application::CalendarStorePort {
   public:
    explicit ConcurrentReplayStore(voicelife::application::StoredCalendarEntry existing)
        : existing_(std::move(existing)) {}

    voicelife::Result<std::optional<voicelife::application::StoredCalendarEntry>> FindByRequestId(
        const std::string&) override {
        ++finds_;
        if (finds_ == 1) {
            return voicelife::Result<std::optional<voicelife::application::StoredCalendarEntry>>::Success(std::nullopt);
        }
        return voicelife::Result<std::optional<voicelife::application::StoredCalendarEntry>>::Success(existing_);
    }

    Status SaveScheduleWithTimingTask(const voicelife::application::StoredCalendarEntry&) override {
        return Status::Error(ErrorCode::kConflict, "并发请求已先完成");
    }

   private:
    int finds_ = 0;
    voicelife::application::StoredCalendarEntry existing_;
};

class RecordingTransport final : public voicelife::im::ImTransportPort {
   public:
    Status Send(const voicelife::im::ImGatewayRequest& request) override {
        ++calls;
        last_url = request.url;
        return Status::Ok();
    }
    int calls = 0;
    std::string last_url;
};

class ReadyAudio final : public voicelife::voice::AudioDevicePort {
   public:
    Status Open() override { return Status::Ok(); }
    void Close() override {}
};

class ReadySpeech final : public voicelife::voice::SpeechProviderPort {
   public:
    Status Connect() override { return Status::Ok(); }
    void Disconnect() override {}
};

class McpBridge final : public voicelife::voice::ToolGatewayPort {
   public:
    explicit McpBridge(voicelife::mcp::McpToolGateway& gateway) : gateway_(gateway) {}
    ToolResult Call(const ToolCall& call) override { return gateway_.Call(call); }

   private:
    voicelife::mcp::McpToolGateway& gateway_;
};

}  // namespace

int main() {
    voicelife::platform::InMemoryCalendarStore store;
    voicelife::platform::SequentialIdGenerator ids;
    RecordingNotifications notifications;
    voicelife::application::CalendarApplication calendar(store, notifications, ids);
    voicelife::mcp::McpToolGateway mcp(calendar);

    Check(mcp.ListTools().size() == 1, "MCP 只暴露当前已实现的工具");
    ToolCall create{
        .request_id = "request-1",
        .name = "voicelife.schedule.create",
        .arguments = {{"title", "架构评审"}, {"starts_at", "1785747600"}},
    };
    const ToolResult first = mcp.Call(create);
    Check(first.status.ok(), "创建日程应成功");
    Check(first.output.at("duplicate") == "false", "首次调用不能标记为重复");
    Check(first.output.at("notification_accepted") == "true", "提交后应发布通知意图");
    Check(store.Size() == 1 && notifications.count == 1, "日程与定时任务应原子落入一个条目");

    const ToolResult duplicate = mcp.Call(create);
    Check(duplicate.status.ok() && duplicate.output.at("duplicate") == "true", "重复 request_id 应幂等返回");
    Check(store.Size() == 1 && notifications.count == 1, "幂等重放不能重复写入或重复通知");

    ConcurrentReplayStore replay_store({
        .schedule =
            {
                .id = "schedule-existing",
                .request_id = "request-race",
                .title = "并发架构评审",
                .starts_at = 1785747600,
            },
        .timing_task =
            {
                .id = "task-existing",
                .schedule_id = "schedule-existing",
                .next_trigger_at = 1785747600,
            },
    });
    RecordingNotifications replay_notifications;
    voicelife::application::CalendarApplication replay_calendar(replay_store, replay_notifications, ids);
    const auto replay = replay_calendar.CreateSchedule({
        .request_id = "request-race",
        .title = "并发架构评审",
        .starts_at = 1785747600,
    });
    Check(replay.ok() && replay.value->duplicate, "并发写入冲突应回读为幂等结果");
    Check(replay.value->schedule_id == "schedule-existing", "并发幂等结果应返回先完成的日程");
    Check(replay_notifications.count == 0, "并发重放不能重复发布通知");

    voicelife::platform::InMemoryCalendarStore degraded_store;
    RejectingNotifications rejecting_notifications;
    voicelife::application::CalendarApplication degraded_calendar(degraded_store, rejecting_notifications, ids);
    const auto degraded = degraded_calendar.CreateSchedule({
        .request_id = "request-notification-down",
        .title = "通知降级评审",
        .starts_at = 1785747600,
    });
    Check(degraded.ok() && !degraded.value->notification_accepted, "通知失败不能伪装成本地日程失败");
    Check(degraded_store.Size() == 1, "通知失败后本地日程仍应保留");

    ToolCall invalid = create;
    invalid.request_id = "request-2";
    invalid.arguments["ends_at"] = "1";
    const ToolResult rejected = mcp.Call(invalid);
    Check(rejected.status.code == ErrorCode::kInvalidArgument, "结束时间早于开始时间应被领域规则拒绝");

    ReadyAudio audio;
    ReadySpeech speech;
    McpBridge bridge(mcp);
    voicelife::voice::VoiceSessionCoordinator voice(audio, speech, bridge);
    Check(voice.Start().ok(), "语音协调器应通过 Port 启动");
    ToolCall second = create;
    second.request_id = "request-3";
    second.arguments["title"] = "代码复审";
    Check(voice.DispatchToolCall(second).status.ok(), "语音模块应只转发工具调用");

    RecordingTransport transport;
    voicelife::im::ImGatewayAdapter gateway(transport);
    Check(gateway.Configure("http://gateway.local", "secret").code == ErrorCode::kInvalidArgument,
          "携带凭据的 IM Gateway 不得使用明文 HTTP");
    Check(gateway.Configure("https://gateway.local/", "secret").ok(), "HTTPS Gateway 配置应通过");

    std::cout << "PASS VoiceLife clean architecture smoke test\n";
    return 0;
}
