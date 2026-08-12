#include "platform_assemblies.h"

namespace voicelife::runtime {

voicelife::voice::PresentationPort& VoiceLifePcbAssembly::presentation() { return ssd1306_adapter_; }

voicelife::voice::PresentationPort& SparkBotAssembly::presentation() { return sparkbot_adapter_; }

}  // namespace voicelife::runtime
