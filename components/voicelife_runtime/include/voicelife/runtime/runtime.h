#pragma once

#include "voicelife/contracts/status.h"
#include "voicelife/runtime/platform_assembly.h"

namespace voicelife::runtime {

/**
 * @brief 初始化并启动设备运行时。
 *
 * 显示语义经注入的 PlatformAssembly -> PresentationPort 提交；Runtime
 * 不直接引用任何具体显示实现或板型。
 * @param assembly 构建期选定的平台装配。
 * @return 运行时启动结果。
 */
Status Start(PlatformAssembly& assembly);

/**
 * @brief 请求取消当前板端语音轮次。
 *
 * 已确认的按键、触摸或其他本地输入适配器调用此入口；运行时负责
 * 失效旧音频代次并恢复本地唤醒待机。
 * @return 请求被当前交互状态接受时返回成功。
 */
Status RequestInterrupt();

}  // namespace voicelife::runtime
