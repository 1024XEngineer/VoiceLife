#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::runtime {
/** @brief Runtime 组合根公开的平台装配前置声明。 */
class PlatformAssembly;
}  // namespace voicelife::runtime

namespace voicelife::runtime_esp {

/**
 * @brief 启动 ESP Runtime 适配器及其任务、队列和定时器。
 * @param assembly 构建期选定的平台装配。
 * @return Runtime 启动结果。
 */
Status Start(runtime::PlatformAssembly& assembly);

/**
 * @brief 向 ESP Runtime 适配器投递取消当前语音回合的请求。
 * @return 请求投递结果。
 */
Status RequestInterrupt();

}  // namespace voicelife::runtime_esp
