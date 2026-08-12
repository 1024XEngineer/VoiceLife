#include "voicelife/im/im_endpoint.h"

#include <cctype>
#include <string>
#include <string_view>

namespace voicelife::im {
namespace {

bool IsValidPort(std::string_view port) {
    if (port.empty() || port.size() > 5) return false;
    unsigned value = 0;
    for (const unsigned char character : port) {
        if (std::isdigit(character) == 0) return false;
        value = value * 10U + static_cast<unsigned>(character - '0');
    }
    return value > 0 && value <= 65535;
}

bool IsValidDnsName(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const std::size_t separator = host.find('.', label_start);
        const std::size_t label_end = separator == std::string_view::npos ? host.size() : separator;
        const std::string_view label = host.substr(label_start, label_end - label_start);
        if (label.empty() || label.size() > 63 || std::isalnum(static_cast<unsigned char>(label.front())) == 0 ||
            std::isalnum(static_cast<unsigned char>(label.back())) == 0) {
            return false;
        }
        for (const unsigned char character : label) {
            if (std::isalnum(character) == 0 && character != '-') return false;
        }
        if (separator == std::string_view::npos) return true;
        label_start = separator + 1;
    }
    return false;
}

}  // namespace

bool IsHttpsGatewayUrl(const std::string& base_url) {
    constexpr std::string_view kHttpsPrefix = "https://";
    if (base_url.rfind(kHttpsPrefix, 0) != 0) return false;

    const std::string_view authority(base_url.data() + kHttpsPrefix.size(), base_url.size() - kHttpsPrefix.size());
    if (authority.empty() || authority.find_first_of("/?#@") != std::string_view::npos) return false;
    for (const unsigned char character : authority) {
        if (std::iscntrl(character) != 0 || std::isspace(character) != 0) return false;
    }
    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos || closing == 1) return false;
        for (const unsigned char character : authority.substr(1, closing - 1)) {
            if (std::isxdigit(character) == 0 && character != ':' && character != '.') return false;
        }
        if (closing + 1 == authority.size()) return true;
        return authority[closing + 1] == ':' && IsValidPort(authority.substr(closing + 2));
    }
    const auto port_separator = authority.find(':');
    if (port_separator == std::string_view::npos) return IsValidDnsName(authority);
    if (authority.find(':', port_separator + 1) != std::string_view::npos) return false;
    return IsValidDnsName(authority.substr(0, port_separator)) && IsValidPort(authority.substr(port_separator + 1));
}

}  // namespace voicelife::im
