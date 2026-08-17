#include "mcp_worker_policy.h"

#include "support/test_support.h"

int main() {
    using voicelife::im::kImHttpRequestTimeoutMs;
    using voicelife::runtime::kMcpResponseGraceMs;
    using voicelife::runtime::kMcpResponseTimeoutMs;
    using voicelife::test::Check;

    Check(kMcpResponseTimeoutMs > kImHttpRequestTimeoutMs, "MCP 不应在仍可能成功的 IM HTTP 请求之前超时");
    Check(kMcpResponseGraceMs >= 2000U, "MCP 应为网络请求完成后的调度与结果传递预留时间");
    return 0;
}
