#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace voicelife::runtime {

using WifiProvisioningStatusSink = std::function<void(std::string_view title, std::string_view detail)>;

/** Credentials submitted from the local SoftAP provisioning page. */
struct WifiProvisioningCredentials {
    std::string ssid;
    std::string password;
};

/** Why the device entered the local provisioning flow. */
enum class WifiProvisioningCause {
    kMissingCredentials,
    kConnectionFailed,
    kUserRequested,
};

/** State that is safe to expose to presentation and transport adapters. */
enum class WifiProvisioningPhase {
    kInactive,
    kServing,
    kValidating,
    kSucceeded,
    kTimedOut,
};

/**
 * Parses an application/x-www-form-urlencoded SoftAP form body.
 *
 * Only `ssid` and `password` are accepted. The password is deliberately never
 * included in returned error text or presentation state.
 */
std::optional<WifiProvisioningCredentials> ParseWifiProvisioningForm(std::string_view body);

/** A small deterministic state machine used by the ESP HTTP and Wi-Fi adapters. */
class WifiProvisioningSession {
   public:
    void Start(WifiProvisioningCause cause);
    bool Submit(WifiProvisioningCredentials credentials);
    std::optional<WifiProvisioningCredentials> TakePendingCredentials();
    void CompleteValidation(bool connected);
    void Timeout();

    WifiProvisioningPhase phase() const { return phase_; }
    std::optional<WifiProvisioningCause> cause() const { return cause_; }

   private:
    WifiProvisioningPhase phase_ = WifiProvisioningPhase::kInactive;
    std::optional<WifiProvisioningCause> cause_;
    std::optional<WifiProvisioningCredentials> pending_credentials_;
};

}  // namespace voicelife::runtime
#include <functional>
