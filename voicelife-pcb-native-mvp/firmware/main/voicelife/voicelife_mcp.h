#pragma once

namespace voicelife {

class VoiceLifeService;

class VoiceLifeMcpAdapter {
public:
    static void Register(VoiceLifeService& service);
};

}  // namespace voicelife
