#include "voicelife/runtime/runtime.h"

namespace voicelife::runtime {

Runtime::Runtime()
    : im_gateway_(im_transport_),
      calendar_(store_, im_gateway_, ids_),
      mcp_(calendar_),
      mcp_voice_bridge_(mcp_),
      voice_(audio_, speech_, mcp_voice_bridge_) {}

Status Runtime::Start() { return voice_.Start(); }

}  // namespace voicelife::runtime
