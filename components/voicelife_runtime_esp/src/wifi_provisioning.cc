#include "wifi_provisioning.h"

#include <cctype>
#include <utility>

namespace voicelife::runtime {
namespace {

constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMaxPasswordBytes = 64;

std::optional<unsigned char> DecodeHex(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    const auto lower = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(value)));
    if (lower >= 'a' && lower <= 'f') {
        return static_cast<unsigned char>(10 + lower - 'a');
    }
    return std::nullopt;
}

std::optional<std::string> DecodeFormValue(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (current != '%') {
            decoded.push_back(current);
            continue;
        }
        if (index + 2 >= value.size()) {
            return std::nullopt;
        }
        const auto high = DecodeHex(value[index + 1]);
        const auto low = DecodeHex(value[index + 2]);
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>((*high << 4U) | *low));
        index += 2;
    }
    return decoded;
}

bool HasAcceptedLengths(const WifiProvisioningCredentials& credentials) {
    return !credentials.ssid.empty() && credentials.ssid.size() <= kMaxSsidBytes && !credentials.password.empty() &&
           credentials.password.size() <= kMaxPasswordBytes;
}

}  // namespace

std::optional<WifiProvisioningCredentials> ParseWifiProvisioningForm(std::string_view body) {
    WifiProvisioningCredentials credentials;
    bool saw_ssid = false;
    bool saw_password = false;

    size_t field_start = 0;
    while (field_start <= body.size()) {
        const size_t field_end = body.find('&', field_start);
        const std::string_view field = body.substr(field_start, field_end - field_start);
        const size_t separator = field.find('=');
        if (field.empty() || separator == std::string_view::npos) {
            return std::nullopt;
        }

        const std::string_view key = field.substr(0, separator);
        const auto value = DecodeFormValue(field.substr(separator + 1));
        if (!value.has_value()) {
            return std::nullopt;
        }
        if (key == "ssid" && !saw_ssid) {
            credentials.ssid = *value;
            saw_ssid = true;
        } else if (key == "password" && !saw_password) {
            credentials.password = *value;
            saw_password = true;
        } else {
            return std::nullopt;
        }

        if (field_end == std::string_view::npos) {
            break;
        }
        field_start = field_end + 1;
    }

    if (!saw_ssid || !saw_password || !HasAcceptedLengths(credentials)) {
        return std::nullopt;
    }
    return credentials;
}

void WifiProvisioningSession::Start(WifiProvisioningCause cause) {
    cause_ = cause;
    pending_credentials_.reset();
    phase_ = WifiProvisioningPhase::kServing;
}

bool WifiProvisioningSession::Submit(WifiProvisioningCredentials credentials) {
    if (phase_ != WifiProvisioningPhase::kServing || !HasAcceptedLengths(credentials)) {
        return false;
    }
    pending_credentials_ = std::move(credentials);
    phase_ = WifiProvisioningPhase::kValidating;
    return true;
}

std::optional<WifiProvisioningCredentials> WifiProvisioningSession::TakePendingCredentials() {
    auto credentials = std::move(pending_credentials_);
    pending_credentials_.reset();
    return credentials;
}

void WifiProvisioningSession::CompleteValidation(bool connected) {
    if (phase_ != WifiProvisioningPhase::kValidating) {
        return;
    }
    phase_ = connected ? WifiProvisioningPhase::kSucceeded : WifiProvisioningPhase::kServing;
}

void WifiProvisioningSession::Timeout() {
    if (phase_ == WifiProvisioningPhase::kServing || phase_ == WifiProvisioningPhase::kValidating) {
        pending_credentials_.reset();
        phase_ = WifiProvisioningPhase::kTimedOut;
    }
}

}  // namespace voicelife::runtime
