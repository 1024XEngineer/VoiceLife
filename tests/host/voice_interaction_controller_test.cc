#include "voicelife/voice/voice_interaction_controller.h"

#include "support/test_support.h"

using voicelife::ErrorCode;
using voicelife::test::Check;
using voicelife::voice::VoiceInteractionAction;
using voicelife::voice::VoiceInteractionController;
using voicelife::voice::VoiceInteractionEvent;
using voicelife::voice::VoiceInteractionState;

namespace {

void CheckTransition(VoiceInteractionController& controller, VoiceInteractionEvent event,
                     VoiceInteractionState expected_state, VoiceInteractionAction expected_action,
                     const char* message) {
    const auto transition = controller.Handle(event);
    Check(transition.ok() && transition.value.has_value(), message);
    Check(transition.value->state == expected_state, "交互状态迁移错误");
    Check(transition.value->action == expected_action, "交互动作迁移错误");
}

}  // namespace

int main() {
    VoiceInteractionController controller;
    Check(controller.state() == VoiceInteractionState::kBooting, "控制器应以 BOOT 状态启动");
    CheckTransition(controller, VoiceInteractionEvent::kTransportConnected, VoiceInteractionState::kBooting,
                    VoiceInteractionAction::kNone, "启动前 Provider 已连接只能确认网络，不能跳过 boot 转场");
    CheckTransition(controller, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "启动后应进入待机并启动本地唤醒");
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "本地唤醒应开始一轮云端语音");
    CheckTransition(controller, VoiceInteractionEvent::kCaptureStarted, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kNone, "采集开始不应改变聆听状态");
    CheckTransition(controller, VoiceInteractionEvent::kIntentReceived, VoiceInteractionState::kThinking,
                    VoiceInteractionAction::kNone, "识别文本或工具调用后应显示思考");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "TTS 开始后应显示播报");
    CheckTransition(controller, VoiceInteractionEvent::kInterruptRequested, VoiceInteractionState::kInterrupting,
                    VoiceInteractionAction::kInterruptSession, "板端打断应先中止会话");
    CheckTransition(controller, VoiceInteractionEvent::kInterruptCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "打断完成后应恢复本地待机");

    VoiceInteractionController terminal_controller;
    CheckTransition(terminal_controller, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "终结型回复测试应先进入待机");
    CheckTransition(terminal_controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "终结型回复应从有效语音回合开始");
    CheckTransition(terminal_controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "终结型回复应允许进入播报状态");
    CheckTransition(terminal_controller, VoiceInteractionEvent::kTerminalResponseCompleted,
                    VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                    "绑定码等终结型回复播报后应直接回待机，不进入 follow-up 聆听");

    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "新一轮唤醒应可开始");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "无需先收到文本也允许服务器直接开始 TTS");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStopped, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartCapture, "TTS 结束后应进入 follow-up 聆听供用户续说");
    CheckTransition(controller, VoiceInteractionEvent::kPressUp, VoiceInteractionState::kFinalizing,
                    VoiceInteractionAction::kStopVoiceTurn, "follow-up 聆听可通过松开触摸进入等待最终 STT");
    CheckTransition(controller, VoiceInteractionEvent::kFinalizationTimedOut, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "最终 STT 超时应恢复待机");

    VoiceInteractionController restart_during_finalization;
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "重开语音用例应先完成启动");
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kWakeDetected,
                    VoiceInteractionState::kListening, VoiceInteractionAction::kStartVoiceTurn,
                    "重开语音用例应先进入聆听");
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kPressUp, VoiceInteractionState::kFinalizing,
                    VoiceInteractionAction::kStopVoiceTurn, "松开后应进入最终识别等待");
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kPressDown, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kInterruptAndStartCapture,
                    "等待最终识别时再次按下必须取消旧回合并立即开始新采集");

    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "按住说打断路径前应可进入一轮语音");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "按住说打断路径应可进入播报状态");
    CheckTransition(controller, VoiceInteractionEvent::kPressDown, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kInterruptAndStartCapture,
                    "播报中按住说只能重启采集，不能伪造本地唤醒事件");
    CheckTransition(controller, VoiceInteractionEvent::kPressUp, VoiceInteractionState::kFinalizing,
                    VoiceInteractionAction::kStopVoiceTurn, "打断后松开触摸应进入等待最终 STT");

    CheckTransition(controller, VoiceInteractionEvent::kTransportDisconnected, VoiceInteractionState::kReconnecting,
                    VoiceInteractionAction::kRestoreStandby, "断线时必须停止云端上行并保留本地待机");
    CheckTransition(controller, VoiceInteractionEvent::kTransportConnected, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "重连完成后应重新可被唤醒");

    CheckTransition(controller, VoiceInteractionEvent::kPressDown, VoiceInteractionState::kOpeningCapture,
                    VoiceInteractionAction::kStartCapture, "触摸按下应提交采集请求（事务式启动）");
    CheckTransition(controller, VoiceInteractionEvent::kCaptureStarted, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kNone, "capture_started 确认后才进入聆听中");
    CheckTransition(controller, VoiceInteractionEvent::kPressUp, VoiceInteractionState::kFinalizing,
                    VoiceInteractionAction::kStopVoiceTurn, "触摸松开应进入等待最终 STT");
    CheckTransition(controller, VoiceInteractionEvent::kFinalizationTimedOut, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "触摸松开后最终 STT 超时应恢复待机");
    CheckTransition(controller, VoiceInteractionEvent::kTransportDisconnected, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "空闲后的服务端有序关闭必须保持本地可唤醒，不显示重连中");
    CheckTransition(controller, VoiceInteractionEvent::kTransportConnected, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kNone, "后台重连完成不得扰动空闲显示");

    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "待机唤醒仍应开始云端语音");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "打断回归路径应允许直接播报");
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kInterrupting,
                    VoiceInteractionAction::kInterruptSession, "播报中再次唤醒应只打断当前播报");
    CheckTransition(controller, VoiceInteractionEvent::kInterruptCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "播报打断后应回到待机再等待下一次唤醒");

    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "待机中唤醒应开始新一轮云端语音");
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kStopVoiceTurn, "聆听中再次唤醒应关闭当前音频通道");

    const auto invalid = controller.Handle(VoiceInteractionEvent::kTtsStopped);
    Check(invalid.status.code == ErrorCode::kConflict && controller.state() == VoiceInteractionState::kStandby,
          "乱序 TTS stop 不能破坏待机状态");
    const auto failure = controller.Handle(VoiceInteractionEvent::kFailure);
    Check(failure.ok() && failure.value->state == VoiceInteractionState::kError &&
              failure.value->action == VoiceInteractionAction::kInterruptSession,
          "失败必须中止远端轮次后恢复本地待机");
    CheckTransition(controller, VoiceInteractionEvent::kStandbyReady, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kNone, "本地待机恢复后应清除错误状态");
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kStartVoiceTurn, "错误恢复后仍应可重新唤醒");
    CheckTransition(controller, VoiceInteractionEvent::kInterruptRequested, VoiceInteractionState::kInterrupting,
                    VoiceInteractionAction::kInterruptSession, "重新唤醒后仍应支持打断");
    CheckTransition(controller, VoiceInteractionEvent::kStandbyReady, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kNone, "打断失败后的物理待机也应能收口状态");
    const auto repeated_failure = controller.Handle(VoiceInteractionEvent::kFailure);
    Check(repeated_failure.ok() && repeated_failure.value->action == VoiceInteractionAction::kInterruptSession,
          "待机故障首次发生时应尝试中止远端轮次");
    const auto settled_failure = controller.Handle(VoiceInteractionEvent::kFailure);
    Check(settled_failure.ok() && settled_failure.value->action == VoiceInteractionAction::kNone &&
              controller.state() == VoiceInteractionState::kError,
          "重复待机故障不得无限排队");
    return 0;
}
