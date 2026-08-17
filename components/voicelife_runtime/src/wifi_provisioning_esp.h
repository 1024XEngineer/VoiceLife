#pragma once

#include <string>
#include <vector>

#include "voicelife/contracts/status.h"
#include "wifi_provisioning.h"

namespace voicelife::runtime {

/**
 * Starts a time-limited, WPA2-protected SoftAP captive portal and waits for one
 * candidate Wi-Fi credential pair. It never persists the candidate.
 */
Result<WifiProvisioningCredentials> ProvisionWifiOverSoftAp(WifiProvisioningCause cause,
                                                            const std::vector<std::string>& discovered_ssids,
                                                            const WifiProvisioningStatusSink& status_sink);

}  // namespace voicelife::runtime
