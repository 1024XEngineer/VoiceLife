#pragma once

#include "voicelife/contracts/status.h"

namespace voicelife::mcp {
/** @brief 提供 MCP 工具注册能力的服务端。 */
class McpServer;
}  // namespace voicelife::mcp

namespace voicelife::schedule {
/** @brief 提供日程业务用例的服务。 */
class ScheduleService;
}  // namespace voicelife::schedule

namespace voicelife::mcp {

/** @brief 将日程 Use Case 注册为 MCP 工具；不持有业务状态或存储资源。
 * @param server MCP 工具服务端。
 * @param service 日程业务服务。
 * @return 注册结果。
 */
Status RegisterScheduleTools(McpServer& server, schedule::ScheduleService& service);

}  // namespace voicelife::mcp
