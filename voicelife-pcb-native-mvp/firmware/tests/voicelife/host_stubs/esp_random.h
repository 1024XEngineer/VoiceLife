#pragma once

#include <stdint.h>

static inline uint32_t esp_random(void) {
    static uint32_t value = 0x13579bdfu;
    value = value * 1664525u + 1013904223u;
    return value;
}
