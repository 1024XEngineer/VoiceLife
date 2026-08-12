std::string PartitionTableJson() {
    std::string result = "[";
    esp_partition_iterator_t iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    bool first = true;
    while (iterator != nullptr) {
        const esp_partition_t* partition = esp_partition_get(iterator);
        if (partition != nullptr) {
            if (!first) result += ',';
            first = false;
            result += "{\"label\":\"" + std::string(partition->label) +
                      "\",\"type\":" + std::to_string(partition->type) +
                      ",\"subtype\":" + std::to_string(partition->subtype) +
                      ",\"address\":" + std::to_string(partition->address) +
                      ",\"size\":" + std::to_string(partition->size) + '}';
        }
        iterator = esp_partition_next(iterator);
    }
    // esp_partition_next releases the iterator itself once it reaches the end.
    esp_partition_iterator_release(iterator);
    return result + ']';
}

Result<linx::LinxOtaDeviceInfo> ReadOtaDeviceInfo() {
    if (const Status wifi_status = EnsureWifiStaConnected(); !wifi_status.ok()) {
        return Result<linx::LinxOtaDeviceInfo>::Failure(wifi_status.code, wifi_status.message);
    }
    wifi_ap_record_t access_point{};
    if (const esp_err_t error = esp_wifi_sta_get_ap_info(&access_point); error != ESP_OK) {
        return Result<linx::LinxOtaDeviceInfo>::Failure(ErrorCode::kUnavailable, "Linx OTA 需要已连接的 Wi-Fi STA");
    }
    auto client_id = LoadOrCreateLinxClientId();
    if (!client_id.ok() || !client_id.value.has_value()) {
        return Result<linx::LinxOtaDeviceInfo>::Failure(client_id.status.code, client_id.status.message);
    }
    uint32_t flash_size = 0;
    if (const esp_err_t error = esp_flash_get_size(nullptr, &flash_size); error != ESP_OK) {
        return Result<linx::LinxOtaDeviceInfo>::Failure(EspError("读取 Flash 容量", error).code,
                                                        EspError("读取 Flash 容量", error).message);
    }
    const esp_app_desc_t* application = esp_app_get_description();
    const std::string device_id = ReadLinxOtaDeviceId();
    const std::string partitions = PartitionTableJson();
    if (application == nullptr || device_id.empty() || partitions == "[]") {
        return Result<linx::LinxOtaDeviceInfo>::Failure(ErrorCode::kUnavailable, "Linx OTA 设备信息不可用");
    }
    const size_t ssid_size = strnlen(reinterpret_cast<const char*>(access_point.ssid), sizeof(access_point.ssid));
    uint32_t psram_size = 0;
#if CONFIG_SPIRAM
    psram_size = static_cast<uint32_t>(esp_psram_get_size());
#endif
    return Result<linx::LinxOtaDeviceInfo>::Success(linx::LinxOtaDeviceInfo{
        .activation_version = "1",
        .device_id = device_id,
        .client_id = std::move(*client_id.value),
        .user_agent = std::string(kBoardName) + '/' + application->version,
        .application_name = kBoardName,
        .application_version = application->version,
        .application_elf_sha256 = EncodeLinxOtaHexDigest(application->app_elf_sha256, sizeof(application->app_elf_sha256)),
        .chip_model_name = "esp32s3",
        .flash_size_bytes = flash_size,
        .psram_size_bytes = psram_size,
        .partition_table_json = partitions,
        .board_type = kBoardName,
        .board_name = kBoardName,
        .wifi_ssid = std::string(reinterpret_cast<const char*>(access_point.ssid), ssid_size),
        .wifi_rssi = access_point.rssi,
        .serial_number = std::nullopt,
        .accept_language = std::string("zh-CN")});
}

esp_err_t CollectOtaResponse(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0) return ESP_OK;
    auto* response = static_cast<std::string*>(event->user_data);
    if (response->size() + static_cast<size_t>(event->data_len) > kMaxOtaResponseBytes) return ESP_ERR_NO_MEM;
    response->append(static_cast<const char*>(event->data), event->data_len);
    return ESP_OK;
}
