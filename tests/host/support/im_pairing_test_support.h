#pragma once

#include <string>
#include <utility>
#include <vector>

#include "support/test_support.h"
#include "voicelife/contracts/im/pairing_session.h"
#include "voicelife/im/im_pairing_client.h"

class FakePairingPort final : public voicelife::im::ImPairingPort {
   public:
    voicelife::im::PairingCreateResult created;
    std::vector<voicelife::im::PairingQueryResult> queried;

    voicelife::im::PairingCreateResult Create(const voicelife::im::PairingCreateOptions&) override { return created; }
    voicelife::im::PairingQueryResult Query(const std::string&) override {
        voicelife::test::Check(!queried.empty(), "假配对端口必须预置查询响应");
        auto result = queried.front();
        queried.erase(queried.begin());
        return result;
    }
};

inline voicelife::contracts::im::CreatedPairingSession CreatedSession(std::string created_at, std::string expires_at) {
    return {.session = {.id = "pairing-1",
                        .userId = "user-fixture",
                        .deviceId = "device-fixture",
                        .allowedPlatforms = std::vector<std::string>{"wechat_official"},
                        .status = "pending",
                        .expiresAt = std::move(expires_at),
                        .createdAt = std::move(created_at),
                        .confirmedAt = std::nullopt},
            .displayCode = "123456"};
}
