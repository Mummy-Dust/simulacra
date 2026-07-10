#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
static inline uint32_t esp_random(void){ return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }
static inline void esp_fill_random(void *buf, size_t n){
    unsigned char *p = buf;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)rand();
}
