#include "voicelife/im/im_endpoint.h"

#include <string>

namespace voicelife::im {

bool IsHttpsGatewayUrl(const std::string& base_url) {
    constexpr const char* kHttpsPrefix = "https://";
    if (base_url.rfind(kHttpsPrefix, 0) != 0) {
        return false;
    }
    return base_url.find('?') == std::string::npos && base_url.find('#') == std::string::npos;
}

}  // namespace voicelife::im
