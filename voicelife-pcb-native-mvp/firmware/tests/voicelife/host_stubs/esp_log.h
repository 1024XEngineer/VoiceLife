#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, format, ...) fprintf(stderr, "E [%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fprintf(stderr, "W [%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) fprintf(stderr, "I [%s] " format "\n", tag, ##__VA_ARGS__)
