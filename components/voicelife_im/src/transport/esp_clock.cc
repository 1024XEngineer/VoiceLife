#include "esp_clock.h"

#include <ctime>

#include "esp_log.h"

namespace voicelife::im {
namespace {

constexpr char kTag[] = "voicelife_im";
// 1970 前的时刻无法以 time_t 表示，取一个可靠的哨兵：2035 之前若系统时间
// 仍为 1970，说明 SNTP 尚未同步。
constexpr time_t kSaneEpoch = 2051222400;  // 2035-01-01T00:00:00Z

}  // namespace

std::string EspClock::NowIso() {
    const time_t now = time(nullptr);
    if (now < kSaneEpoch) {
        static bool warned = false;
        if (!warned) {
            ESP_LOGW(kTag, "系统时间未同步（SNTP），动作窗口将按已过期处理");
            warned = true;
        }
        // SNTP 未同步时 time() 返回 1970 纪元值；若按原值上报，任何窗口都判定为
        // “未过期”，与预期相反。改报 2035 哨兵时刻，使所有窗口一律判定过期、
        // 动作流保持关闭，直到时间同步。
        return "2035-01-01T00:00:00.000Z";
    }
    struct tm utc;
    if (gmtime_r(&now, &utc) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);
    // 固定毫秒精度（.000Z），保证 ISO-8601 UTC 字典序即时间序。
    return std::string(buf) + ".000Z";
}

}  // namespace voicelife::im
