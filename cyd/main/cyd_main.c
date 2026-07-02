#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"        // swap to esp_lcd_panel_st7789 if Step 1 says ST7789
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "radar_render.h"

#define PIN_MOSI 13
#define PIN_SCK  14
#define PIN_CS   15
#define PIN_DC    2
#define PIN_RST  (-1)
#define PIN_BL   21
#define LCD_W    240
#define LCD_H    320
static const char *TAG = "cyd";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx){
    (void)io; (void)e; (void)ctx;
    BaseType_t hp = pdFALSE; xSemaphoreGiveFromISR(s_flush_done, &hp); return hp == pdTRUE;
}

// Synchronous flush: blocks until the SPI color transfer completes before returning, so the
// caller (radar_render_view) can safely reuse/mutate its single scratch band buffer for the
// next band. Without this, esp_lcd_panel_draw_bitmap's async transfer races the next clear.
void cyd_flush(int y0, int h, const uint16_t *buf, void *ctx){
    (void)ctx;
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y0 + h, (void*)buf);
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
}

static bool cyd_panel_init(esp_lcd_panel_handle_t *out)
{
    s_flush_done = xSemaphoreCreateBinary();

    ledc_timer_config_t lt = { .speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_8_BIT,
                               .timer_num=LEDC_TIMER_0, .freq_hz=5000, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num=PIN_BL, .speed_mode=LEDC_LOW_SPEED_MODE,
                                 .channel=LEDC_CHANNEL_0, .timer_sel=LEDC_TIMER_0, .duty=255 };
    ledc_channel_config(&lc);

    spi_bus_config_t bus = { .mosi_io_num=PIN_MOSI, .sclk_io_num=PIN_SCK, .miso_io_num=-1,
                             .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=LCD_W*40*2+8 };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = { .dc_gpio_num=PIN_DC, .cs_gpio_num=PIN_CS,
        .pclk_hz=40*1000*1000, .lcd_cmd_bits=8, .lcd_param_bits=8, .spi_mode=0, .trans_queue_depth=10,
        .on_color_trans_done=on_trans_done };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num=PIN_RST, .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR,
                                      .bits_per_pixel=16 };
    if (esp_lcd_new_panel_ili9341(io, &pc, out) != ESP_OK) return false;   // or _st7789
    esp_lcd_panel_reset(*out); esp_lcd_panel_init(*out);
    esp_lcd_panel_invert_color(*out, false);       // flip if colors look inverted
    esp_lcd_panel_disp_on_off(*out, true);
    return true;
}

void app_main(void)
{
    nvs_flash_init();
    if (!cyd_panel_init(&s_panel)) { ESP_LOGE(TAG, "panel init failed"); return; }

    static uint16_t band[LCD_W * 40];
    radar_wire_status_t demo = {0};
    demo.active_devices = 7; demo.uptime_s = 83; demo.threat_count = 2;
    demo.threats[0].hash = 0x1234; demo.threats[0].best_rssi = -41; demo.threats[0].epochs = 6;
    demo.threats[1].hash = 0x9abc; demo.threats[1].best_rssi = -66; demo.threats[1].epochs = 2;
    radar_render_view(RADAR_VIEW_RADAR, &demo, 45, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
    ESP_LOGW(TAG, "panel up: static radar demo drawn");
}
