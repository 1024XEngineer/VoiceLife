#include "voicelife/mcp/mcp_schedule_application.h"
#include "voicelife/mcp/mcp_request_executor.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
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

/** @brief 验证未初始化应用拒绝所有请求。 */
void CheckUninitializedApplication() {
    Repository repository;
    voicelife::mcp::McpScheduleApplication application(repository);
    const auto submitted =
        application.SubmitJsonRpc("{}", [](voicelife::Result<std::string>) {});
    Check(submitted.code == voicelife::ErrorCode::kUnavailable && submitted.message == "MCP 应用尚未初始化",
          "未初始化应用应拒绝提交请求");
}

/** @brief 验证 JSON-RPC 端点错误分支、序列化与转义。 */
void CheckJsonRpcEndpointBranches() {
    Repository repository;
    voicelife::mcp::McpScheduleApplication application(repository);
    Check(application.Initialize().ok(), "端点分支测试应初始化应用");

    const auto missing_method = SubmitAndWait(application, R"({"jsonrpc":"2.0","id":1})");
    Check(missing_method.ok() && missing_method.value->find("\"code\":-32600") != std::string::npos,
          "缺少 method 应返回 -32600");

    const auto unknown_method = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"foo","id":1})");
    Check(unknown_method.ok() && unknown_method.value->find("\"code\":-32601") != std::string::npos,
          "未知方法应返回 -32601");

    const auto initialize = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    Check(initialize.ok() && initialize.value->find("protocolVersion") != std::string::npos,
          "initialize 应返回协议能力");

    const auto missing_name = SubmitAndWait(application,
                                            R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{}})");
    Check(missing_name.ok() && missing_name.value->find("\"code\":-32602") != std::string::npos,
          "tools/call 缺少 name 应返回 -32602");

    const auto bad_arguments = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"schedule.create","arguments":"x"}})");
    Check(bad_arguments.ok() && bad_arguments.value->find("\"code\":-32602") != std::string::npos,
          "arguments 非对象应返回 -32602");

    const auto float_argument = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"schedule.create","arguments":{"event":"浮点","start_time":1.5}}})");
    Check(float_argument.ok() && float_argument.value->find("\"code\":-32602") != std::string::npos,
          "浮点参数应被拒绝");

    const auto tool_error = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"schedule.create","arguments":{}}})");
    Check(tool_error.ok() && tool_error.value->find("\"code\":-32602") != std::string::npos,
          "工具参数缺失应返回 -32602");

    const auto unknown_tool = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"schedule.unknown","arguments":{}}})");
    Check(unknown_tool.ok() && unknown_tool.value->find("\"code\":-32601") != std::string::npos,
          "未知工具应返回 -32601");

    // event 覆盖全部转义分支：\" \\ \b \f \n \r \t 与 \u0001 控制字符。
    const auto escaped = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"esc","params":{"name":"schedule.create","arguments":{"event":"a\"b\\c\nd\te\rf\bg\fh\u0001i","start_time":1800000000}}})");
    Check(escaped.ok() && escaped.value.has_value(), "含转义字符的请求应执行成功");
    // Escape 输出即 JSON 字符串字面量内容：\" \\ \n \t \r \b \f 与 \u0001。
    Check(escaped.value->find("event=a\\\"b\\\\c\\nd\\te\\rf\\bg\\fh\\u0001i") != std::string::npos,
          "响应文本应正确转义特殊字符");

    // id 各类型触发 Serialize 分支。
    const auto bool_id = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"tools/list","id":true})");
    Check(bool_id.ok() && bool_id.value->find("\"id\":true") != std::string::npos, "布尔 id 应序列化为 true");
    const auto number_id = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"tools/list","id":1.5})");
    Check(number_id.ok() && number_id.value->find("\"id\":1.5") != std::string::npos, "浮点 id 应序列化");
    const auto array_id = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"tools/list","id":[1,2]})");
    Check(array_id.ok() && array_id.value->find("\"id\":[1,2]") != std::string::npos, "数组 id 应序列化");
    const auto object_id = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"tools/list","id":{"a":1,"b":2}})");
    Check(object_id.ok() && object_id.value->find("\"id\":{\"a\":1,\"b\":2}") != std::string::npos,
          "对象 id 应序列化");
    const auto null_id = SubmitAndWait(application, R"({"jsonrpc":"2.0","method":"tools/list","id":null})");
    Check(null_id.ok() && null_id.value->find("\"id\":null") != std::string::npos, "null id 应序列化为 null");
}

/** @brief 验证工具执行观察器与摘要分支。 */
void CheckExecutionObserver() {
    Repository repository;
    voicelife::mcp::McpScheduleApplication application(repository);
    Check(application.Initialize().ok(), "观察器测试应初始化应用");

    bool started = false;
    bool finished = false;
    bool finished_success = false;
    std::string finished_summary;
    application.SetExecutionObserver([&](bool started_flag, bool success, std::string_view summary) {
        if (started_flag) {
            started = true;
        } else {
            finished = true;
            finished_success = success;
            finished_summary = std::string(summary);
        }
    });

    const auto created = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"obs-1","params":{"name":"schedule.create","arguments":{"event":"观察器","start_time":1800000000}}})");
    Check(created.ok() && started && finished && finished_success && finished_summary == "日程已创建",
          "成功的 schedule.create 应报告已创建摘要");

    const auto failed = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"obs-2","params":{"name":"schedule.create","arguments":{}}})");
    Check(failed.ok() && finished_summary == "日程创建失败", "失败的 schedule.create 应报告失败摘要");

    const auto queried = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"obs-3","params":{"name":"schedule.query","arguments":{}}})");
    Check(queried.ok() && finished_summary == "日程查询完成", "schedule.query 应报告查询完成摘要");

    const auto unknown = SubmitAndWait(
        application,
        R"({"jsonrpc":"2.0","method":"tools/call","id":"obs-4","params":{"name":"schedule.unknown","arguments":{}}})");
    Check(unknown.ok() && finished_summary == "日程操作失败", "未知工具应报告操作失败摘要");
}

/** @brief 验证请求执行器的拒绝分支：空回调、未启动、队列满与停止后。 */
void CheckExecutorRejection() {
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool release = false;
    int handled = 0;
    voicelife::mcp::McpRequestExecutor executor([&](std::string_view) {
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            ++handled;
            gate.wait(lock, [&] { return release; });
        }
        return voicelife::Result<std::string>::Success(std::string("ok"));
    });

    bool not_started_sink_called = false;
    const auto not_started =
        executor.Submit("req", [&](voicelife::Result<std::string>) { not_started_sink_called = true; });
    Check(not_started.code == voicelife::ErrorCode::kUnavailable && not_started_sink_called,
          "未启动执行器应拒绝请求并回调响应");

    const auto no_sink = executor.Submit("req", {});
    Check(no_sink.code == voicelife::ErrorCode::kInvalidArgument, "空响应回调应被拒绝");

    Check(executor.Start().ok(), "执行器应启动");
    const auto first_request = executor.Submit("req", [&](voicelife::Result<std::string>) {});
    Check(first_request.ok(), "首个请求应入队");
    // 等待 worker 取走首个请求并阻塞在 handler，保证队列容量语义稳定。
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        Check(gate.wait_for(lock, std::chrono::seconds(1), [&] { return handled >= 1; }),
              "执行器 worker 应开始消费请求");
    }
    int queued_responses = 0;
    voicelife::Status within_capacity = voicelife::Status::Ok();
    for (int index = 0; index < 4; ++index) {
        within_capacity =
            executor.Submit("req", [&](voicelife::Result<std::string>) { ++queued_responses; });
        if (!within_capacity.ok()) break;
    }
    Check(within_capacity.ok(), "容量内请求应入队");
    const auto overflow = executor.Submit("req", [&](voicelife::Result<std::string>) { ++queued_responses; });
    Check(overflow.code == voicelife::ErrorCode::kUnavailable, "队列满时应拒绝请求");

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate.notify_all();
    executor.Stop();
    Check(handled >= 1, "执行器应消费过请求");

    bool stopped_sink_called = false;
    const auto stopped = executor.Submit("req", [&](voicelife::Result<std::string>) { stopped_sink_called = true; });
    Check(stopped.code == voicelife::ErrorCode::kUnavailable && stopped_sink_called,
          "停止后执行器应拒绝请求并回调响应");
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
    CheckUninitializedApplication();
    CheckJsonRpcEndpointBranches();
    CheckExecutionObserver();
    CheckExecutorRejection();
    return 0;
}
