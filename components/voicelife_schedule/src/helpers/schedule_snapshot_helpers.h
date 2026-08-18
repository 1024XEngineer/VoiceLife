#pragma once

#include <string>

#include "voicelife/schedule/schedule_types.h"

namespace voicelife::schedule {

/**
 * @brief 将日程序列化为操作快照 JSON 字符串。
 *
 * 快照用于业务层回滚：把一次 update / cancel 之前的实体状态完整保存，
 * 时间以 Unix 秒整数存储以保证无损往返，字段结构由本模块维护。
 * @param schedule 待序列化的日程。
 * @return 紧凑 JSON 字符串；操作模块不解析，仅按字节存取。
 */
std::string SerializeScheduleSnapshot(const Schedule& schedule);

}  // namespace voicelife::schedule
