#include "voicelife_im_http.h"

#include "network_interface.h"

#include <utility>

namespace voicelife {
namespace {

constexpr size_t kMaxResponseBody = 64 * 1024;

std::string JoinUrl(const std::string& base, const std::string& path) {
    if (base.empty()) return {};
    if (path.empty()) return base;
    if (base.back() == '/' && path.front() == '/') return base.substr(0, base.size() - 1) + path;
    if (base.back() != '/' && path.front() != '/') return base + "/" + path;
    return base + path;
}

}  // namespace

VoiceLifeImHttpTransport::VoiceLifeImHttpTransport(NetworkInterface* network,
                                                   std::string base_url,
                                                   std::string token)
    : network_(network), base_url_(std::move(base_url)), token_(std::move(token)) {}

ImHttpResponse VoiceLifeImHttpTransport::Request(const std::string& method,
                                                 const std::string& path,
                                                 const std::string& body) {
    ImHttpResponse response;
    if (network_ == nullptr || base_url_.empty() || token_.empty()) {
        response.error = "IM HTTP transport is not configured";
        return response;
    }
    auto http = network_->CreateHttp(3);
    if (!http) {
        response.error = "failed to create HTTP client";
        return response;
    }
    http->SetTimeout(5000);
    http->SetKeepAlive(false);
    http->SetHeader("Authorization", "Bearer " + token_);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json");
    if (!body.empty()) http->SetContent(std::string(body));
    if (!http->Open(method, JoinUrl(base_url_, path))) {
        response.error = "HTTP open failed";
        response.status_code = http->GetLastError();
        return response;
    }
    response.transport_ok = true;
    response.status_code = http->GetStatusCode();
    response.body = http->ReadAll();
    if (response.body.size() > kMaxResponseBody) {
        response.transport_ok = false;
        response.error = "HTTP response too large";
        response.body.clear();
    }
    http->Close();
    return response;
}

}  // namespace voicelife
