#pragma once

#include <string>

#include "voicelife/im/im_clock.h"

namespace voicelife::im {

/// 基于系统 wall-clock 的 ISO-8601 UTC 时钟。仅固件编译。
///
/// 依赖板级 SNTP 时间同步；未同步时返回 1970 时刻，动作窗口会被判定为
/// 已过期，命令回传 expired 终态。
class EspClock : public ImClock {
   public:
    /**
     * @brief 返回当前 UTC 时间的 ISO-8601 表示。
     * @return 固定毫秒精度（.000Z）以保证字典序即时间序。
     */
    std::string NowIso() override;
};

}  // namespace voicelife::im
