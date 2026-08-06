#pragma once

#include <string>

namespace voicelife::im {

/// 时间端口：以 ISO-8601 UTC 提供当前时间，供动作窗口与命令有效期判断。
class ImClock {
   public:
    /** @brief 允许通过接口指针释放时钟。 */
    virtual ~ImClock() = default;
    /**
     * @brief 返回当前 UTC 时间的 ISO-8601 表示。
     * @return 例如 "2026-08-03T00:00:00.000Z"。
     */
    virtual std::string NowIso() = 0;
};

}  // namespace voicelife::im
