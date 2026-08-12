#include "voicelife/im/im_provisioning.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace voicelife::im {
namespace {

constexpr std::array<uint8_t, 3> kMagicPrefix = {'V', 'L', 'I'};
constexpr std::size_t kMaxGatewayOriginBytes = 255;
constexpr std::size_t kMaxDeviceIdBytes = 128;
constexpr std::size_t kMaxDeviceTokenBytes = 512;
constexpr std::size_t kMaxUserIdBytes = 128;

std::size_t DecodeSize(std::span<const uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::size_t>(bytes[offset]) << 8U) | bytes[offset + 1];
}

bool IsSafeText(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t character) {
        return character != 0 && std::iscntrl(static_cast<unsigned char>(character)) == 0;
    });
}

bool IsSafeCredential(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](uint8_t character) { return character > 0x20U && character < 0x7fU; });
}

std::string ToString(std::span<const uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

Result<ImProvisioningHeader> ParseImProvisioningHeader(std::span<const uint8_t> bytes) {
    if (bytes.size() < kImProvisioningHeaderSize) {
        return Result<ImProvisioningHeader>::Failure(ErrorCode::kInvalidArgument, "IM provisioning header 不完整");
    }
    if (!std::equal(kMagicPrefix.begin(), kMagicPrefix.end(), bytes.begin()) ||
        (bytes[3] != static_cast<uint8_t>('1') && bytes[3] != static_cast<uint8_t>('2'))) {
        return Result<ImProvisioningHeader>::Failure(ErrorCode::kInvalidArgument, "IM provisioning magic 无效");
    }

    ImProvisioningHeader header;
    header.allow_overwrite = bytes[3] == static_cast<uint8_t>('2');
    header.gateway_origin_size = DecodeSize(bytes, 4);
    header.device_id_size = DecodeSize(bytes, 6);
    header.device_token_size = DecodeSize(bytes, 8);
    header.user_id_size = DecodeSize(bytes, 10);
    if (header.gateway_origin_size == 0 || header.gateway_origin_size > kMaxGatewayOriginBytes ||
        header.device_id_size == 0 || header.device_id_size > kMaxDeviceIdBytes || header.device_token_size == 0 ||
        header.device_token_size > kMaxDeviceTokenBytes || header.user_id_size > kMaxUserIdBytes) {
        return Result<ImProvisioningHeader>::Failure(ErrorCode::kInvalidArgument, "IM provisioning 字段长度越界");
    }
    header.payload_size =
        header.gateway_origin_size + header.device_id_size + header.device_token_size + header.user_id_size;
    return Result<ImProvisioningHeader>::Success(header);
}

Result<ImProvisioningRequest> ParseImProvisioningRequest(std::span<const uint8_t> bytes) {
    auto header = ParseImProvisioningHeader(bytes);
    if (!header.ok() || !header.value.has_value()) {
        return Result<ImProvisioningRequest>::Failure(header.status.code, header.status.message);
    }
    if (bytes.size() != kImProvisioningHeaderSize + header.value->payload_size) {
        return Result<ImProvisioningRequest>::Failure(ErrorCode::kInvalidArgument, "IM provisioning frame 长度不匹配");
    }

    std::size_t offset = kImProvisioningHeaderSize;
    const auto take = [&bytes, &offset](std::size_t size) {
        const auto field = bytes.subspan(offset, size);
        offset += size;
        return field;
    };
    const auto origin = take(header.value->gateway_origin_size);
    const auto device_id = take(header.value->device_id_size);
    const auto token = take(header.value->device_token_size);
    const auto user_id = take(header.value->user_id_size);
    if (!IsSafeText(origin) || !IsSafeCredential(device_id) || !IsSafeCredential(token) || !IsSafeText(user_id)) {
        return Result<ImProvisioningRequest>::Failure(ErrorCode::kInvalidArgument,
                                                      "IM provisioning 字段包含非法控制字符");
    }

    return Result<ImProvisioningRequest>::Success({.allow_overwrite = header.value->allow_overwrite,
                                                   .gateway_origin = ToString(origin),
                                                   .device_id = ToString(device_id),
                                                   .device_token = ToString(token),
                                                   .user_id = ToString(user_id)});
}

}  // namespace voicelife::im
