#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "voicelife/linx/linx_types.h"

namespace voicelife::linx_esp {

class SecretResolverPort {
   public:
    virtual ~SecretResolverPort() = default;
    virtual Result<std::string> Resolve(std::string_view reference) = 0;
};

enum class TransportState {
    kDisconnected,
    kConnecting,
    kConnected,
    kReconnecting,
    kFailed,
};

struct EspWebSocketTransportOptions {
    size_t max_message_bytes = 16 * 1024;
    size_t event_queue_capacity = 8;
    size_t event_chunk_bytes = 4096;
    uint32_t connect_timeout_ms = 10000;
    uint32_t network_timeout_ms = 10000;
    uint32_t reconnect_timeout_ms = 1000;
    bool enable_close_reconnect = true;
    bool allow_insecure_ws = false;
};

// ESP-IDF implementation of the Linx transport. ESP-IDF headers stay in the
// .cc file; callers only see the Linx port and platform-neutral options.
class EspWebSocketTransport final : public linx::LinxTransportPort {
   public:
    EspWebSocketTransport(SecretResolverPort& secrets,
                          EspWebSocketTransportOptions options = {});
    ~EspWebSocketTransport() override;

    EspWebSocketTransport(const EspWebSocketTransport&) = delete;
    EspWebSocketTransport& operator=(const EspWebSocketTransport&) = delete;

    Status Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink) override;
    Status SendText(std::string_view message) override;
    Status SendAudio(const voice::AudioFrame& frame) override;
    Status Close() override;
    void SetGeneration(uint64_t generation) override;

    [[nodiscard]] TransportState state() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voicelife::linx_esp
