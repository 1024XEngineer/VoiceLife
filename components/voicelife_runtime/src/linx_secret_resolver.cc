#include "linx_secret_resolver.h"

#ifdef ESP_PLATFORM

#include <string>
#include <string_view>
#include <utility>

#include "linx_ota_bootstrap.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace voicelife::runtime {
namespace {

#if CONFIG_NVS_ENCRYPTION
Result<std::string> ReadNvsString(nvs_handle_t handle, const char* key) {
    size_t required = 0;
    esp_err_t error = nvs_get_str(handle, key, nullptr, &required);
    if (error != ESP_OK || required <= 1) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, std::string("缺少 Linx NVS 配置: ") + key);
    }
    std::string value(required, '\0');
    error = nvs_get_str(handle, key, value.data(), &required);
    if (error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kUnavailable, "读取 Linx NVS 配置失败");
    }
    value.resize(required > 0 ? required - 1 : 0);
    if (value.empty()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, std::string("Linx NVS 配置为空: ") + key);
    }
    return Result<std::string>::Success(std::move(value));
}
#endif

}  // namespace

Result<std::string> NvsSecretResolver::Resolve(std::string_view reference) {
#if !CONFIG_NVS_ENCRYPTION
    (void)reference;
    return Result<std::string>::Failure(ErrorCode::kUnavailable, "Linx token 解析需要启用 NVS encryption");
#else
    constexpr std::string_view prefix = "nvs://";
    if (reference.rfind(prefix, 0) != 0) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx token 引用必须使用 nvs://");
    }
    const std::string path(reference.substr(prefix.size()));
    const auto separator = path.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= path.size()) {
        return Result<std::string>::Failure(ErrorCode::kInvalidArgument, "Linx token 引用格式无效");
    }
    nvs_handle_t handle = 0;
    const esp_err_t open_error =
        nvs_open_from_partition(LinxSecretPartitionLabel(), path.substr(0, separator).c_str(), NVS_READONLY, &handle);
    if (open_error != ESP_OK) {
        return Result<std::string>::Failure(ErrorCode::kNotFound, "Linx token NVS 命名空间不可用");
    }
    auto result = ReadNvsString(handle, path.substr(separator + 1).c_str());
    nvs_close(handle);
    return result;
#endif
}

}  // namespace voicelife::runtime

#endif
