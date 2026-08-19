#include "voicelife/linx_esp/esp_websocket_transport.h"

#include <utility>

#include "esp_websocket_impl.h"

namespace voicelife::linx_esp {

EspWebSocketTransport::EspWebSocketTransport(SecretResolverPort& secrets, EspWebSocketTransportOptions options)
    : impl_(std::make_unique<Impl>(secrets, std::move(options))) {}

EspWebSocketTransport::~EspWebSocketTransport() = default;

Status EspWebSocketTransport::Connect(const linx::LinxConnectionConfig& config, linx::LinxTransportSink sink) {
    return impl_->Connect(config, std::move(sink));
}

Status EspWebSocketTransport::SendText(std::string_view message) { return impl_->SendText(message); }

Status EspWebSocketTransport::SendAudio(voice::AudioFrame frame) { return impl_->SendAudio(std::move(frame)); }

Status EspWebSocketTransport::Close() { return impl_->Close(); }

void EspWebSocketTransport::SetGeneration(uint64_t generation) { impl_->SetGeneration(generation); }

TransportState EspWebSocketTransport::state() const { return impl_->state(); }

}  // namespace voicelife::linx_esp
