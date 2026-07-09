#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h){ (void)ns;(void)m; *h = 0; return -1; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *l){ (void)h;(void)k;(void)v;(void)l; return -1; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l){ (void)h;(void)k;(void)v;(void)l; return 0; }
static inline esp_err_t nvs_commit(nvs_handle_t h){ (void)h; return 0; }
static inline void nvs_close(nvs_handle_t h){ (void)h; }
