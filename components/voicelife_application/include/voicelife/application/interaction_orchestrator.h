#pragma once

#include <cstdint>

namespace voicelife::application {

/** @brief 交互应用层接收的跨域事件分类，不携带平台 SDK 类型。 */
enum class InteractionEventKind : uint8_t {
    kBootstrapRequested,
    kBoardInputArrived,
    kVoiceLifecycleChanged,
    kConnectivityChanged,
};

/** @brief 一次交互编排请求的稳定输入模型。 */
struct InteractionEvent {
    InteractionEventKind kind = InteractionEventKind::kBootstrapRequested;
};

/** @brief 交互编排器发给 Runtime Adapter 的稳定动作分类。 */
enum class InteractionActionKind : uint8_t {
    kInitializeInteraction,
    kDispatchBoardInput,
    kDispatchVoiceLifecycle,
    kRefreshConnectivity,
};

/** @brief 一次交互编排请求产生的动作。 */
struct InteractionAction {
    InteractionActionKind kind = InteractionActionKind::kInitializeInteraction;

    friend constexpr bool operator==(InteractionAction lhs, InteractionAction rhs) { return lhs.kind == rhs.kind; }
};

/** @brief Runtime Adapter 实现的动作接收端口。 */
class InteractionActionSink {
   public:
    /** @brief 虚析构函数。 */
    virtual ~InteractionActionSink() = default;
    /** @brief 接收一个应用层编排动作。 @param action 要执行的语义动作。 */
    virtual void Submit(InteractionAction action) = 0;
};

/**
 * @brief 平台无关的交互应用服务。
 *
 * 该骨架只定义跨域事件与动作的编排边界。既有 Runtime 事件循环仍保持原状；
 * 后续迁移必须先用行为轨迹证明等价，才能将现有事件接入此服务。
 */
class InteractionOrchestrator {
   public:
    /**
     * @brief 将一个跨域事件映射为一个确定的 Runtime Adapter 动作。
     * @param event 要编排的跨域交互事件。
     * @param actions 用于记录或执行动作的 Runtime Adapter 端口。
     */
    void Handle(InteractionEvent event, InteractionActionSink& actions) const;
};

}  // namespace voicelife::application
