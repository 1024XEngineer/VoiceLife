#include "voicelife/runtime/runtime.h"

#include "voicelife/runtime_esp/esp_runtime.h"

namespace voicelife::runtime {

Status Start(PlatformAssembly& assembly) { return runtime_esp::Start(assembly); }

Status RequestInterrupt() { return runtime_esp::RequestInterrupt(); }

}  // namespace voicelife::runtime
