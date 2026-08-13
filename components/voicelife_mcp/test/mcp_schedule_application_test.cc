#include "voicelife/mcp/mcp_schedule_application.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "support/test_support.h"
#include "voicelife/contracts/json.h"
#include "voicelife/schedule/schedule_repository.h"

using voicelife::test::Check;

namespace {

class Repository final : public voicelife::schedule::ScheduleRepository {
   public:
    voicelife::Result<voicelife::schedule::Schedule> Insert(const voicelife::schedule::Schedule& value) override {
        auto stored = value;
        stored.id = 7;
        schedules.push_back(stored);
        return voicelife::Result<voicelife::schedule::Schedule>::Success(stored);
    }
    voicelife::Result<std::vector<voicelife::schedule::Schedule>> FindAll() const override {
        return voicelife::Result<std::vector<voicelife::schedule::Schedule>>::Success(schedules);
    }
    voicelife::Result<std::optional<voicelife::schedule::Schedule>> FindByIdempotencyKey(
        std::string_view key) const override {
        const auto found = requests.find(std::string(key));
        return voicelife::Result<std::optional<voicelife::schedule::Schedule>>::Success(
            found == requests.end() ? std::nullopt : std::optional<voicelife::schedule::Schedule>(found->second));
    }
    voicelife::Result<voicelife::schedule::Schedule> InsertOnce(const voicelife::schedule::Schedule& value,
                                                                std::string_view key) override {
        const auto found = requests.find(std::string(key));
        if (found != requests.end()) return voicelife::Result<voicelife::schedule::Schedule>::Success(found->second);
        const auto stored = Insert(value);
        if (stored.ok()) requests.emplace(std::string(key), *stored.value);
        return stored;
    }
    std::vector<voicelife::schedule::Schedule> schedules;
    std::unordered_map<std::string, voicelife::schedule::Schedule> requests;
};

voicelife::JsonValue Parse(const std::string& text) {
    voicelife::JsonValue value;
    Check(voicelife::ParseJson(text, value).ok(), "MCP JSON-RPC 响应必须可解析");
    return value;
}

voicelife::Result<std::string> SubmitAndWait(voicelife::mcp::McpScheduleApplication& application,
                                             std::string_view request) {
    std::mutex mutex;
    std::condition_variable completed;
    bool done = false;
    voicelife::Result<std::string> response =
        voicelife::Result<std::string>::Failure(voicelife::ErrorCode::kUnavailable, "MCP 请求未完成");
    Check(application
              .SubmitJsonRpc(request,
                             [&](voicelife::Result<std::string> value) {
                                 {
                                     std::lock_guard<std::mutex> lock(mutex);
                                     response = std::move(value);
                                     done = true;
                                 }
                                 completed.notify_one();
                             })
              .ok(),
          "MCP 请求必须可提交到专属执行器");
    std::unique_lock<std::mutex> lock(mutex);
    Check(completed.wait_for(lock, std::chrono::seconds(1), [&] { return done; }), "MCP 请求必须在限时内完成");
    return response;
}

}  // namespace

int main() {
    Repository repository;
    voicelife::mcp::McpScheduleApplication application(repository);
    Check(application.Initialize().ok(), "独立 MCP 日程应用必须初始化成功");

    const auto listed = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"tools/list","id":1})");
    Check(listed.ok() && listed.value.has_value(), "tools/list 必须由 MCP 组件处理");
    const auto list_json = Parse(*listed.value);
    Check(list_json.Get("result")->Get("tools")->array.size() == 2, "MCP 必须注册日程创建和查询工具");

    const auto created = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"create-1","params":{"name":"schedule.create","arguments":{"event":"项目评审","start_time":1800000000}}})");
    Check(created.ok() && created.value.has_value() && repository.schedules.size() == 1,
          "MCP 创建必须写入注入的 Repository，而非模拟结果");

    const auto replayed = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"create-1","params":{"name":"schedule.create","arguments":{"event":"项目评审","start_time":1800000000}}})");
    Check(replayed.ok() && replayed.value.has_value() && repository.schedules.size() == 1 &&
              replayed.value->find("id=7") != std::string::npos,
          "同一 MCP request id 重试必须回放首次创建结果，不得创建第二条日程");

    const auto queried = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"query-1","params":{"name":"schedule.query","arguments":{"keyword":"项目评审"}}})");
    Check(queried.ok() && queried.value.has_value() && queried.value->find("项目评审") != std::string::npos,
          "MCP 查询必须读取同一 Repository 中创建的日程");

    const auto malformed = SubmitAndWait(application, "{");
    Check(
        malformed.ok() && malformed.value.has_value() && malformed.value->find("\"code\":-32700") != std::string::npos,
        "无效 JSON 必须返回标准 JSON-RPC parse error");

    std::mutex notification_mutex;
    std::condition_variable notification_completed;
    bool notification_done = false;
    bool notification_response = false;
    Check(application
              .SubmitJsonRpc(R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})",
                             [&](voicelife::Result<std::string> value) {
                                 {
                                     std::lock_guard<std::mutex> lock(notification_mutex);
                                     notification_response =
                                         value.ok() && value.value.has_value() && !value.value->empty();
                                     notification_done = true;
                                 }
                                 notification_completed.notify_one();
                             })
              .ok(),
          "notification 必须可提交");
    std::unique_lock<std::mutex> notification_lock(notification_mutex);
    Check(notification_completed.wait_for(notification_lock, std::chrono::seconds(1),
                                          [&] { return notification_done; }) &&
              !notification_response,
          "notification 不得产生 JSON-RPC 响应");
    return 0;
}
