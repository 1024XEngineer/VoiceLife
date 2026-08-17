#include "wifi_provisioning.h"

#include <string>

#include "support/test_support.h"

int main() {
    using voicelife::runtime::ParseWifiProvisioningForm;
    using voicelife::runtime::WifiProvisioningCause;
    using voicelife::runtime::WifiProvisioningPhase;
    using voicelife::runtime::WifiProvisioningSession;

    const auto credentials = ParseWifiProvisioningForm("ssid=Office+WiFi&password=s%40fe%2Bpass");
    voicelife::test::Check(credentials.has_value(), "valid form is accepted");
    voicelife::test::Check(credentials->ssid == "Office WiFi", "SSID is URL decoded");
    voicelife::test::Check(credentials->password == "s@fe+pass", "password is URL decoded");

    voicelife::test::Check(!ParseWifiProvisioningForm("ssid=Office"), "password is required");
    voicelife::test::Check(!ParseWifiProvisioningForm("ssid=&password=secret123"), "empty SSID is rejected");
    voicelife::test::Check(!ParseWifiProvisioningForm("ssid=Office&password=%ZZ"), "invalid escapes are rejected");
    voicelife::test::Check(!ParseWifiProvisioningForm("ssid=Office&password=secret123&ssid=Other"),
                           "duplicate fields are rejected");
    voicelife::test::Check(!ParseWifiProvisioningForm("ssid=Office&password=secret123&extra=1"),
                           "unexpected fields are rejected");

    WifiProvisioningSession session;
    session.Start(WifiProvisioningCause::kConnectionFailed);
    voicelife::test::Check(session.phase() == WifiProvisioningPhase::kServing, "session starts serving");
    voicelife::test::Check(session.cause() == WifiProvisioningCause::kConnectionFailed, "cause is retained");
    voicelife::test::Check(session.Submit(*credentials), "first candidate is accepted");
    voicelife::test::Check(session.phase() == WifiProvisioningPhase::kValidating,
                           "candidate switches session to validation");
    voicelife::test::Check(!session.Submit(*credentials), "candidate cannot be replaced while validating");

    const auto pending = session.TakePendingCredentials();
    voicelife::test::Check(pending.has_value() && pending->ssid == "Office WiFi", "candidate can be consumed once");
    voicelife::test::Check(!session.TakePendingCredentials(), "candidate is not retained after consumption");
    session.CompleteValidation(false);
    voicelife::test::Check(session.phase() == WifiProvisioningPhase::kServing,
                           "failed candidate returns to serving without persistence");
    voicelife::test::Check(session.Submit(*credentials), "another candidate can be submitted after failure");
    session.CompleteValidation(true);
    voicelife::test::Check(session.phase() == WifiProvisioningPhase::kSucceeded,
                           "verified candidate completes the session");
    session.Timeout();
    voicelife::test::Check(session.phase() == WifiProvisioningPhase::kSucceeded,
                           "completed session cannot be timed out");
    return 0;
}
