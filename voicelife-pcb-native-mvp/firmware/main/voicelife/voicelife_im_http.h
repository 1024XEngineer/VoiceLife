#pragma once

#include "voicelife_im_sync.h"

#include <string>

class NetworkInterface;

namespace voicelife {

class VoiceLifeImHttpTransport final : public ImTransport {
public:
    VoiceLifeImHttpTransport(NetworkInterface* network, std::string base_url, std::string token);

    ImHttpResponse Request(const std::string& method, const std::string& path,
                           const std::string& body) override;

private:
    NetworkInterface* network_ = nullptr;
    std::string base_url_;
    std::string token_;
};

}  // namespace voicelife
