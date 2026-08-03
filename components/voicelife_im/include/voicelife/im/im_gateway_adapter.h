#pragma once

#include <string>

#include "voicelife/application/calendar_application.h"

namespace voicelife::im {

struct ImGatewayRequest {
    std::string url;
    std::string bearer_token;
    application::NotificationIntent intent;
};

class ImTransportPort {
   public:
    virtual ~ImTransportPort() = default;
    // HTTP and JSON belong in the concrete transport adapter, not in the semantic IM adapter.
    virtual Status Send(const ImGatewayRequest& request) = 0;
};

class ImGatewayAdapter final : public application::NotificationPort {
   public:
    explicit ImGatewayAdapter(ImTransportPort& transport) : transport_(transport) {}

    Status Configure(std::string base_url, std::string bearer_token);
    Status Publish(const application::NotificationIntent& intent) override;
    [[nodiscard]] bool configured() const { return !base_url_.empty() && !bearer_token_.empty(); }

   private:
    ImTransportPort& transport_;
    std::string base_url_;
    std::string bearer_token_;
};

}  // namespace voicelife::im
