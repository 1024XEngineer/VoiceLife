// #235 轮询任务代次租约：旧任务退出不能吞掉新会话的轮询请求。

#include <cstdint>

#include "im_binding_polling_lease.h"
#include "support/test_support.h"

using voicelife::runtime::BindingPollingLease;
using voicelife::test::Check;

namespace {

void TestFirstSessionCreatesAWorkerAndDuplicateDoesNot() {
    BindingPollingLease lease;
    Check(lease.Acquire(7), "没有轮询任务时，第一个 pending 会话必须创建任务");
    Check(!lease.Acquire(7), "同一会话重复结果不得创建第二个轮询任务");
    Check(lease.generation() == 7, "租约必须保留当前会话代次");
}

void TestOldWorkerCannotReleaseSessionAdoptedDuringExit() {
    BindingPollingLease lease;
    Check(lease.Acquire(7), "旧会话应先创建轮询任务");
    Check(!lease.Acquire(8), "旧任务存活时，新会话应由该任务接管而非并发创建");
    Check(lease.generation() == 8, "新会话必须接管轮询租约");
    Check(!lease.Release(7), "旧任务不得释放已经移交给新会话的租约");
    Check(lease.Release(8), "当前拥有者退出时必须释放租约");
}

}  // namespace

int main() {
    TestFirstSessionCreatesAWorkerAndDuplicateDoesNot();
    TestOldWorkerCannotReleaseSessionAdoptedDuringExit();
    return 0;
}
