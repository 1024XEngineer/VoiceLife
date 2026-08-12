#include "voicelife/display_sparkbot/display_snapshot_queue.h"

#include "support/test_support.h"
#include "voicelife/voice/display_snapshot.h"

using voicelife::test::Check;
using voicelife::voice::DisplaySnapshot;

namespace {

DisplaySnapshot MakeSnapshot(uint64_t revision) {
    DisplaySnapshot snapshot;
    snapshot.revision = revision;
    return snapshot;
}

}  // namespace

int main() {
    using voicelife::display_sparkbot::DisplaySnapshotQueue;

    // 有界容量：Push 超过容量时丢弃最旧（最新状态优先）。
    DisplaySnapshotQueue queue(3);
    Check(queue.capacity() == 3 && queue.empty(), "容量 3 且初始为空");
    Check(queue.Push(MakeSnapshot(1)) && queue.Push(MakeSnapshot(2)) && queue.Push(MakeSnapshot(3)), "前三帧必须入队");
    Check(queue.size() == 3, "队列满");
    Check(queue.Push(MakeSnapshot(4)), "第四帧必须被接受（丢弃最旧）");
    Check(queue.size() == 3, "满后入队仍保持容量上限");

    DisplaySnapshot out;
    Check(queue.Pop(&out) && out.revision == 2, "最旧快照（revision 2）必须被丢弃后移出");
    Check(out.revision == 2, "丢弃的是 revision 1，队首应为 revision 2");

    // 非阻塞 Pop 与空队列。
    DisplaySnapshotQueue empty_queue(2);
    Check(!empty_queue.Pop(&out), "空队列 Pop 必须返回 false");

    // 阻塞 WaitPop：先等后推。
    DisplaySnapshotQueue blocking_queue(2);
    bool pushed = false;
    bool popped = false;
    {
        // 单线程下 WaitPop 有超时：超时返回 false。
        DisplaySnapshot first;
        Check(!blocking_queue.WaitPop(&first, 10), "空队列 WaitPop 超时返回 false");
    }
    pushed = blocking_queue.Push(MakeSnapshot(9));
    popped = blocking_queue.WaitPop(&out, 100);
    Check(pushed && popped && out.revision == 9, "入队后 WaitPop 必须取到快照");

    return 0;
}
