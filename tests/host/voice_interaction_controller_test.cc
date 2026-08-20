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

void CompleteWakeAcknowledgement(VoiceInteractionController& controller, const char* message) {
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kAcknowledging,
                    VoiceInteractionAction::kStartVoiceTurn, message);
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "确认播报开始后应进入播报态");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStopped, VoiceInteractionState::kOpeningCapture,
                    VoiceInteractionAction::kStartCapture, "确认播报结束后应事务式请求采集");
    CheckTransition(controller, VoiceInteractionEvent::kCaptureStarted, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kNone, "只有采集成功确认后才能显示聆听中");
}

}  // namespace

int main() {
    VoiceInteractionController controller;
    Check(controller.state() == VoiceInteractionState::kBooting, "控制器应以 BOOT 状态启动");
    CheckTransition(controller, VoiceInteractionEvent::kTransportConnected, VoiceInteractionState::kBooting,
                    VoiceInteractionAction::kNone, "启动前 Provider 已连接只能确认网络，不能跳过 boot 转场");
    CheckTransition(controller, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "启动后应进入待机并启动本地唤醒");

    VoiceInteractionController acknowledgement_timeout_controller;
    CheckTransition(acknowledgement_timeout_controller, VoiceInteractionEvent::kBootCompleted,
                    VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                    "确认超时用例应先进入待机");
    CheckTransition(acknowledgement_timeout_controller, VoiceInteractionEvent::kWakeDetected,
                    VoiceInteractionState::kAcknowledging, VoiceInteractionAction::kStartVoiceTurn,
                    "确认播报请求期间不得伪装为聆听中");
    CheckTransition(acknowledgement_timeout_controller, VoiceInteractionEvent::kAcknowledgementTimedOut,
                    VoiceInteractionState::kOpeningCapture, VoiceInteractionAction::kStartCapture,
                    "确认播报未产生首段音频必须立即开麦，不能永久卡住");
    CheckTransition(acknowledgement_timeout_controller, VoiceInteractionEvent::kCaptureStarted,
                    VoiceInteractionState::kListening, VoiceInteractionAction::kNone,
                    "确认超时后的采集成功确认才进入聆听");

    VoiceInteractionController acknowledgement_audio_timeout_controller;
    CheckTransition(acknowledgement_audio_timeout_controller, VoiceInteractionEvent::kBootCompleted,
                    VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                    "确认首音频超时用例应先进入待机");
    CheckTransition(acknowledgement_audio_timeout_controller, VoiceInteractionEvent::kWakeDetected,
                    VoiceInteractionState::kAcknowledging, VoiceInteractionAction::kStartVoiceTurn,
                    "确认首音频超时用例必须先请求远端确认");
    CheckTransition(acknowledgement_audio_timeout_controller, VoiceInteractionEvent::kTtsStarted,
                    VoiceInteractionState::kSpeaking, VoiceInteractionAction::kNone,
                    "远端仅确认开始时应进入 speaking 等待首段 PCM");
    CheckTransition(acknowledgement_audio_timeout_controller, VoiceInteractionEvent::kAcknowledgementTimedOut,
                    VoiceInteractionState::kOpeningCapture, VoiceInteractionAction::kStartCapture,
                    "远端 TTS 无首段 PCM 时必须中止等待并开始采集");
    CompleteWakeAcknowledgement(controller, "本地唤醒必须先进入确认播报阶段");
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
    CompleteWakeAcknowledgement(terminal_controller, "终结型回复应从完整唤醒确认开始");
    CheckTransition(terminal_controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "终结型回复应允许进入播报状态");
    CheckTransition(terminal_controller, VoiceInteractionEvent::kTerminalResponseCompleted,
                    VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                    "绑定码等终结型回复播报后应直接回待机，不进入 follow-up 聆听");

    CompleteWakeAcknowledgement(controller, "新一轮唤醒应可完整开始");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "无需先收到文本也允许服务器直接开始 TTS");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStopped, VoiceInteractionState::kOpeningCapture,
                    VoiceInteractionAction::kStartCapture, "TTS 结束后必须等待 follow-up 采集确认");
    CheckTransition(controller, VoiceInteractionEvent::kCaptureStarted, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kNone, "follow-up 采集成功后才进入聆听");
    CheckTransition(controller, VoiceInteractionEvent::kPressUp, VoiceInteractionState::kFinalizing,
                    VoiceInteractionAction::kStopVoiceTurn, "follow-up 聆听可通过松开触摸进入等待最终 STT");
    CheckTransition(controller, VoiceInteractionEvent::kFinalizationTimedOut, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "最终 STT 超时应恢复待机");

    VoiceInteractionController restart_during_finalization;
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "重开语音用例应先完成启动");
    CompleteWakeAcknowledgement(restart_during_finalization, "重开语音用例应先完成确认再聆听");
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kPressUp, VoiceInteractionState::kFinalizing,
                    VoiceInteractionAction::kStopVoiceTurn, "松开后应进入最终识别等待");
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kPressDown,
                    VoiceInteractionState::kInterrupting, VoiceInteractionAction::kInterruptAndStartCapture,
                    "等待最终识别时再次按下必须先取消旧回合，不能提前显示新采集");
    CheckTransition(restart_during_finalization, VoiceInteractionEvent::kCaptureStarted,
                    VoiceInteractionState::kListening, VoiceInteractionAction::kNone,
                    "旧回合取消后只有成功采集确认才能进入新聆听");

    CompleteWakeAcknowledgement(controller, "按住说打断路径前应可进入一轮语音");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "按住说打断路径应可进入播报状态");
    CheckTransition(controller, VoiceInteractionEvent::kPressDown, VoiceInteractionState::kInterrupting,
                    VoiceInteractionAction::kInterruptAndStartCapture,
                    "播报中按住说必须先显示打断中，不能伪造本地唤醒或提前显示聆听");
    CheckTransition(controller, VoiceInteractionEvent::kCaptureStarted, VoiceInteractionState::kListening,
                    VoiceInteractionAction::kNone, "打断后的采集确认才进入聆听");
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

    CompleteWakeAcknowledgement(controller, "待机唤醒仍应完成确认并开始云端语音");
    CheckTransition(controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "打断回归路径应允许直接播报");
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kInterrupting,
                    VoiceInteractionAction::kInterruptSession, "播报中再次唤醒应只打断当前播报");
    CheckTransition(controller, VoiceInteractionEvent::kInterruptCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "播报打断后应回到待机再等待下一次唤醒");

    CompleteWakeAcknowledgement(controller, "待机中唤醒应完成确认并开始新一轮云端语音");
    CheckTransition(controller, VoiceInteractionEvent::kWakeDetected, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kStopVoiceTurn, "聆听中再次唤醒应关闭当前音频通道");

    VoiceInteractionController interrupt_ack_controller;
    CheckTransition(interrupt_ack_controller, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "打断确认用例应先进入待机");
    CompleteWakeAcknowledgement(interrupt_ack_controller, "打断确认用例应先开始一轮语音");
    CheckTransition(interrupt_ack_controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                    VoiceInteractionAction::kNone, "打断确认用例应进入播报");
    CheckTransition(interrupt_ack_controller, VoiceInteractionEvent::kInterruptAndAcknowledge,
                    VoiceInteractionState::kInterrupting, VoiceInteractionAction::kInterruptAndStartVoiceTurn,
                    "确认请求未获 Provider 接受前不得提前显示聆听");
    CheckTransition(interrupt_ack_controller, VoiceInteractionEvent::kInterruptAcknowledged,
                    VoiceInteractionState::kListening, VoiceInteractionAction::kNone,
                    "Provider 接受确认请求后才进入聆听");

    const auto invalid = controller.Handle(VoiceInteractionEvent::kTtsStopped);
    Check(invalid.status.code == ErrorCode::kConflict && controller.state() == VoiceInteractionState::kStandby,
          "乱序 TTS stop 不能破坏待机状态");
    const auto failure = controller.Handle(VoiceInteractionEvent::kFailure);
    Check(failure.ok() && failure.value->state == VoiceInteractionState::kError &&
              failure.value->action == VoiceInteractionAction::kInterruptSession,
          "失败必须中止远端轮次后恢复本地待机");
    CheckTransition(controller, VoiceInteractionEvent::kStandbyReady, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kNone, "本地待机恢复后应清除错误状态");
    CompleteWakeAcknowledgement(controller, "错误恢复后仍应可重新唤醒");
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

    VoiceInteractionController multi_turn_controller;
    CheckTransition(multi_turn_controller, VoiceInteractionEvent::kBootCompleted, VoiceInteractionState::kStandby,
                    VoiceInteractionAction::kRestoreStandby, "多轮测试应先进入待机");
    for (int turn = 0; turn < 24; ++turn) {
        CompleteWakeAcknowledgement(multi_turn_controller, "每轮都必须能从待机开始完整语音会话");
        if (turn % 3 == 0) {
            CheckTransition(multi_turn_controller, VoiceInteractionEvent::kEndpointDetected,
                            VoiceInteractionState::kFinalizing, VoiceInteractionAction::kStopVoiceTurn,
                            "端点检测应在连续多轮中可靠收口采集");
            CheckTransition(multi_turn_controller, VoiceInteractionEvent::kFinalizationTimedOut,
                            VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                            "最终识别超时后下一轮仍必须可开始");
        } else {
            CheckTransition(multi_turn_controller, VoiceInteractionEvent::kTtsStarted, VoiceInteractionState::kSpeaking,
                            VoiceInteractionAction::kNone, "连续多轮中服务端可直接开始播报");
            if (turn % 3 == 1) {
                CheckTransition(multi_turn_controller, VoiceInteractionEvent::kTtsStopped,
                                VoiceInteractionState::kOpeningCapture, VoiceInteractionAction::kStartCapture,
                                "播报结束后应事务式为追问重新打开采集");
                CheckTransition(multi_turn_controller, VoiceInteractionEvent::kCaptureStarted,
                                VoiceInteractionState::kListening, VoiceInteractionAction::kNone,
                                "追问采集确认后才进入聆听");
                CheckTransition(multi_turn_controller, VoiceInteractionEvent::kEndpointDetected,
                                VoiceInteractionState::kFinalizing, VoiceInteractionAction::kStopVoiceTurn,
                                "追问端点应结束本轮采集");
                CheckTransition(multi_turn_controller, VoiceInteractionEvent::kFinalizationTimedOut,
                                VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                                "追问超时后必须回到下一轮起点");
            } else {
                CheckTransition(multi_turn_controller, VoiceInteractionEvent::kInterruptRequested,
                                VoiceInteractionState::kInterrupting, VoiceInteractionAction::kInterruptSession,
                                "连续播报打断必须取消当前会话");
                CheckTransition(multi_turn_controller, VoiceInteractionEvent::kInterruptCompleted,
                                VoiceInteractionState::kStandby, VoiceInteractionAction::kRestoreStandby,
                                "打断完成后不能遗留上一轮播报状态");
            }
        }
    }
    Check(multi_turn_controller.state() == VoiceInteractionState::kStandby, "连续多轮结束后控制器必须回到待机");
    return 0;
}
