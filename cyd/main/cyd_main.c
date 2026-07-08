#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"        // swap to esp_lcd_panel_st7789 if Step 1 says ST7789
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "radar_render.h"
#include "radar_gfx.h"
#include "radar_wire.h"
#include "radar_ui.h"
#include "radar_key.h"
#include "config_wire.h"
#ifdef SIMULACRA_CONFIG_CTRL
#include "sim_ctrl_sk.h"
#endif
#include "learn_wire.h"
#include "learn_db.h"
#include "sig_db.h"
#include "sig_wire.h"
#include "sig_seed.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#define PIN_MOSI 13
#define PIN_SCK  14
#define PIN_CS   15
#define PIN_DC    2
#define PIN_RST  (-1)
#define PIN_BL   21
#define LCD_W    240
#define LCD_H    320
#define TOUCH_IRQ_GPIO 36           // XPT2046 T_IRQ, active-LOW on press (pulled high externally)
#define TOUCH_CLK_GPIO 25
#define TOUCH_CS_GPIO  33
#define TOUCH_DIN_GPIO 32
#define TOUCH_DOUT_GPIO 39          // input-only pin, OK for MISO
#define ESPNOW_CH 1
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
static uint16_t s_txbuf[LCD_W * 40];               // byte-swap scratch (>= one band)
void cyd_flush(int y0, int h, const uint16_t *buf, void *ctx){
    (void)ctx;
    // This CYD's ILI9341 wants big-endian RGB565; our band buffer is native little-endian,
    // so swap each pixel's bytes into the tx scratch before sending.
    int n = LCD_W * h;
    for (int i = 0; i < n; i++) s_txbuf[i] = __builtin_bswap16(buf[i]);
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y0 + h, s_txbuf);
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
    esp_lcd_panel_mirror(*out, true, false);       // this CYD's ILI9341 default is X-mirrored -> un-mirror text
    esp_lcd_panel_disp_on_off(*out, true);
    return true;
}

static void set_backlight(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 255 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---- microSD on its own SPI host (SPI3), separate from the display's SPI2 (E2) ----
#define SD_HOST     SPI3_HOST
#define PIN_SD_MOSI 23
#define PIN_SD_MISO 19
#define PIN_SD_SCK  18
#define PIN_SD_CS    5
#define SD_MOUNT_POINT "/sdcard"
static bool s_sd_ok;
static sdmmc_card_t *s_card;

static bool sd_mount(void)
{
    spi_bus_config_t bus = { .mosi_io_num=PIN_SD_MOSI, .miso_io_num=PIN_SD_MISO,
        .sclk_io_num=PIN_SD_SCK, .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=4096 };
    if (spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGW(TAG,"sd: bus init fail"); return false; }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_SD_CS; dev.host_id = SD_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT(); host.slot = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mnt = { .format_if_mount_failed=false, .max_files=4,
        .allocation_unit_size=16*1024 };
    esp_err_t e = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev, &mnt, &s_card);
    if (e != ESP_OK) { ESP_LOGW(TAG, "sd: absent/unmountable (0x%x) -> RAM-only librarian", e); return false; }
    ESP_LOGW(TAG, "sd: mounted (%lluMB)", ((uint64_t)s_card->csd.capacity)*s_card->csd.sector_size/(1024*1024));
    return true;
}

// ---- ESP-NOW radar link (STA on a fixed channel, broadcast, AES-GCM via radar_wire) ----
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static radar_wire_status_t s_status;             // last good status
static volatile uint32_t   s_status_ms;          // when it arrived (0 = never)
static radar_replay_t      s_replay;
static uint8_t  s_salt[4]; static uint64_t s_ctr;

#define VIGIL_LIB_CAP 128
static learned_template_t s_lib[VIGIL_LIB_CAP];
static size_t             s_lib_count;
static radar_replay_t     s_offer_replay;   // reject replayed LEARN_OFFER
static uint16_t           s_lib_sweep;      // local "time" for merges/age (monotonic tick)

// ---- encrypted-at-rest SD persistence of the RAM library ----
#define LEARN_DB_PATH SD_MOUNT_POINT "/simulacra/learn.db"
#define LEARN_DB_TMP  SD_MOUNT_POINT "/simulacra/learn.tmp"
#define LEARN_DB_SAVE_MS 30000
static uint8_t  s_db_key[32];
static bool     s_lib_dirty;
static uint32_t s_last_offer_ms, s_last_sync_ms, s_last_save_ms;  // 0 = never
static uint32_t s_save_bytes;

// ---- M10 fingerprint signature DB: custodied encrypted on SD, pushed to decoys ----
#define SIG_DB_PATH SD_MOUNT_POINT "/simulacra/threat_sig.db"
#define SIG_DB_TMP  SD_MOUNT_POINT "/simulacra/threat_sig.tmp"
static threat_sig_t s_sigdb[SIG_DB_CAP];
static size_t       s_sigdb_n;
static uint16_t     s_sigdb_ver;
static uint8_t      s_sigdb_key[32];

static void learn_db_load(void)
{
    learn_db_derive_key(SIMULACRA_ESPNOW_KEY, s_db_key);
    if (!s_sd_ok) return;
    FILE *f = fopen(LEARN_DB_PATH, "rb");
    if (!f) { ESP_LOGW(TAG, "learndb: none on card (fresh)"); return; }
    // This build persists only the RAM working set (<= VIGIL_LIB_CAP records), so the buffers
    // are sized to that. A file larger than that comes from a future archive-capable Vigil and
    // is rejected here (rebuilt from sync) rather than partially loaded.
    static uint8_t blob[sizeof(learn_db_hdr_t) + VIGIL_LIB_CAP*sizeof(learned_template_t)];
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 0 || (size_t)fsz > sizeof blob) {
        fclose(f); ESP_LOGW(TAG, "learndb: file exceeds RAM build cap -> rebuild from sync"); return;
    }
    size_t n = fread(blob, 1, (size_t)fsz, f); fclose(f);
    uint16_t cnt = 0;
    static learned_template_t tmp[VIGIL_LIB_CAP];
    if (learn_db_open(blob, n, tmp, &cnt, s_db_key) != 0) {
        ESP_LOGW(TAG, "learndb: open failed (corrupt/foreign) -> rebuild from sync");
        return;
    }
    // Re-gate every record off the card, then merge into the RAM working set (cap VIGIL_LIB_CAP).
    size_t admitted = 0;
    for (uint16_t i = 0; i < cnt; i++)
        if (learn_regate(&tmp[i]) && learn_merge_wire(s_lib, &s_lib_count, VIGIL_LIB_CAP, &tmp[i], s_lib_sweep))
            admitted++;
    ESP_LOGW(TAG, "learndb: loaded %u/%u recs -> lib=%u", (unsigned)admitted, (unsigned)cnt, (unsigned)s_lib_count);
}

static void learn_db_save(void)
{
    if (!s_sd_ok || s_lib_count == 0) return;
    static uint8_t blob[sizeof(learn_db_hdr_t) + VIGIL_LIB_CAP*sizeof(learned_template_t)]; size_t blen;
    if (learn_db_seal(blob, &blen, s_lib, (uint16_t)s_lib_count, s_db_key) != 0) return;
    FILE *f = fopen(LEARN_DB_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "learndb: tmp open fail (keep RAM set)"); return; }
    size_t w = fwrite(blob, 1, blen, f); fclose(f);
    if (w != blen) { ESP_LOGW(TAG, "learndb: short write, abort rename"); remove(LEARN_DB_TMP); return; }
    remove(LEARN_DB_PATH);                       // FAT rename won't clobber; remove then rename
    if (rename(LEARN_DB_TMP, LEARN_DB_PATH) != 0) { ESP_LOGW(TAG, "learndb: rename fail"); return; }
    ESP_LOGW(TAG, "learndb: saved %u recs (%u B)", (unsigned)s_lib_count, (unsigned)blen);
    s_last_save_ms = (uint32_t)(esp_timer_get_time()/1000); s_save_bytes = (uint32_t)blen;
}

static void sig_db_save_card(void)
{
    if (!s_sd_ok || s_sigdb_n == 0) return;
    static uint8_t blob[sizeof(sig_db_hdr_t) + SIG_DB_CAP*sizeof(threat_sig_t)]; size_t blen;
    if (sig_db_seal(blob, &blen, s_sigdb, (uint16_t)s_sigdb_n, s_sigdb_ver, s_sigdb_key) != 0) return;
    FILE *f = fopen(SIG_DB_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "sigdb: tmp open fail"); return; }
    size_t w = fwrite(blob, 1, blen, f); fclose(f);
    if (w != blen) { remove(SIG_DB_TMP); return; }
    remove(SIG_DB_PATH);
    if (rename(SIG_DB_TMP, SIG_DB_PATH) == 0)
        ESP_LOGW(TAG, "sigdb: saved v%u (%u sigs, %u B)", (unsigned)s_sigdb_ver, (unsigned)s_sigdb_n, (unsigned)blen);
}

static void sig_db_init(void)
{
    sig_db_derive_key(SIMULACRA_ESPNOW_KEY, s_sigdb_key);
    s_sigdb_n = sig_seed_copy(s_sigdb, SIG_DB_CAP);   // baseline = compiled seed
    s_sigdb_ver = sig_seed_version();
    if (s_sd_ok) {
        FILE *f = fopen(SIG_DB_PATH, "rb");
        if (f) {
            static uint8_t blob[sizeof(sig_db_hdr_t) + SIG_DB_CAP*sizeof(threat_sig_t)];
            fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
            size_t n = (fsz > 0 && (size_t)fsz <= sizeof blob) ? fread(blob, 1, (size_t)fsz, f) : 0;
            fclose(f);
            static threat_sig_t tmp[SIG_DB_CAP]; uint16_t cnt = 0, ver = 0;   // static: avoid main-task stack overflow
            if (n && sig_db_open(blob, n, tmp, &cnt, &ver, s_sigdb_key) == 0 &&
                ver >= s_sigdb_ver && cnt <= SIG_DB_CAP) {
                memcpy(s_sigdb, tmp, cnt * sizeof(threat_sig_t)); s_sigdb_n = cnt; s_sigdb_ver = ver;
                ESP_LOGW(TAG, "sigdb: loaded v%u (%u sigs) from card", (unsigned)ver, (unsigned)cnt);
            }
        }
    }
    sig_db_save_card();                               // self-populate a fresh/older card with the seed
}

static void broadcast_sig_db(void)
{
    if (s_sigdb_n == 0) return;
    uint8_t chunks = (uint8_t)((s_sigdb_n + SIG_WIRE_RECS_PER_CHUNK - 1) / SIG_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * SIG_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((s_sigdb_n - off < SIG_WIRE_RECS_PER_CHUNK) ? (s_sigdb_n - off)
                                                                             : SIG_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (sig_wire_pack(pl, &plen, &s_sigdb[off], nrec, s_sigdb_ver, ci, chunks) != 0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame, &flen, RADAR_TYPE_SIG_SYNC, pl, plen,
                            SIMULACRA_ESPNOW_KEY, s_salt, ++s_ctr) == 0)
            esp_now_send(BCAST, frame, flen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "sig: broadcast v%u (%u sigs, %u chunks)",
             (unsigned)s_sigdb_ver, (unsigned)s_sigdb_n, (unsigned)chunks);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len){
    (void)info;
    uint8_t type, pl[RADAR_FRAME_MAX], salt[4]; size_t plen; uint64_t ctr;
    if (radar_wire_open(data,(size_t)len,SIMULACRA_ESPNOW_KEY,&type,pl,&plen,salt,&ctr)!=0) return;
    if (type==RADAR_TYPE_STATUS && plen==sizeof(radar_wire_status_t)) {
        if (!radar_replay_ok(&s_replay,salt,ctr)) return;
        memcpy(&s_status, pl, sizeof s_status);
        if (s_status.threat_count > RADAR_MAX_THREATS)      // never trust the wire field: threats[] is fixed-size
            s_status.threat_count = RADAR_MAX_THREATS;      // (a conforming decoy already clamps; this guards the renderer regardless)
        s_status_ms = (uint32_t)(esp_timer_get_time()/1000);
        ESP_LOGW(TAG, "status rx: decoys=%u threats=%u up=%lus",
                 (unsigned)s_status.active_devices, (unsigned)s_status.threat_count,
                 (unsigned long)s_status.uptime_s);
        return;
    }
    if (type==RADAR_TYPE_LEARN_OFFER) {
        if (!radar_replay_ok(&s_offer_replay, salt, ctr)) return;
        learn_chunk_hdr_t h; learned_template_t rx[LEARN_WIRE_RECS_PER_CHUNK]; uint8_t nr;
        if (learn_wire_unpack(pl, plen, rx, &nr, &h) != 0) return;
        for (uint8_t i = 0; i < nr; i++)
            if (learn_regate(&rx[i]))
                if (learn_merge_wire(s_lib, &s_lib_count, VIGIL_LIB_CAP, &rx[i], s_lib_sweep))
                    s_lib_dirty = true;
        s_last_offer_ms = (uint32_t)(esp_timer_get_time()/1000);
        ESP_LOGW(TAG, "offer rx: +%u recs, lib=%u", (unsigned)nr, (unsigned)s_lib_count);
        return;
    }
}
static void send_request(void){
    uint8_t nonce[4]; esp_fill_random(nonce,4);
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame,&flen,RADAR_TYPE_REQUEST,nonce,4,SIMULACRA_ESPNOW_KEY,s_salt,++s_ctr)==0)
        for (int i=0;i<4;i++) esp_now_send(BCAST,frame,flen);
}
#ifdef SIMULACRA_CONFIG_CTRL
static void send_config(uint8_t preset)
{
    uint64_t ctr = ++s_ctr;
    uint8_t nonce12[12]; memcpy(nonce12, s_salt, 4);
    for (int i = 0; i < 8; i++) nonce12[4+i] = (uint8_t)(ctr >> (56 - 8*i));
    config_cmd_t cmd = { .version = CONFIG_WIRE_VER, .preset_id = preset };
    uint8_t pl[CONFIG_WIRE_PAYLOAD_LEN];
    if (config_wire_pack_signed(pl, sizeof pl, &cmd, nonce12, SIMULACRA_CTRL_SK) < 0) return;
    uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
    if (radar_wire_seal(frame, &flen, RADAR_TYPE_CONFIG, pl, sizeof pl,
                        SIMULACRA_ESPNOW_KEY, s_salt, ctr) == 0)
        for (int i = 0; i < 4; i++) esp_now_send(BCAST, frame, flen);
    ESP_LOGW(TAG, "sent CONFIG preset %u", (unsigned)preset);
}
#endif
static void broadcast_library(void){
    if (s_lib_count == 0) return;
    s_lib_sweep++;
    static learned_template_t sel[LEARN_SYNC_TOP_N];
    size_t n = learn_top_n(s_lib, s_lib_count, sel, LEARN_SYNC_TOP_N);
    uint8_t chunks = (uint8_t)((n + LEARN_WIRE_RECS_PER_CHUNK - 1) / LEARN_WIRE_RECS_PER_CHUNK);
    for (uint8_t ci = 0; ci < chunks; ci++) {
        size_t off = (size_t)ci * LEARN_WIRE_RECS_PER_CHUNK;
        uint8_t nrec = (uint8_t)((n - off < LEARN_WIRE_RECS_PER_CHUNK) ? (n - off) : LEARN_WIRE_RECS_PER_CHUNK);
        uint8_t pl[RADAR_FRAME_MAX]; size_t plen;
        if (learn_wire_pack(pl, &plen, &sel[off], nrec, 1, ci, chunks) != 0) continue;
        uint8_t frame[RADAR_FRAME_MAX]; size_t flen;
        if (radar_wire_seal(frame, &flen, RADAR_TYPE_LEARN_SYNC, pl, plen,
                            SIMULACRA_ESPNOW_KEY, s_salt, ++s_ctr) == 0)
            esp_now_send(BCAST, frame, flen);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_last_sync_ms = (uint32_t)(esp_timer_get_time()/1000);
    ESP_LOGW(TAG, "broadcast top-%u of %u recs", (unsigned)n, (unsigned)s_lib_count);
}
static uint32_t age_s(uint32_t now, uint32_t ts){ return ts ? (uint32_t)(now - ts)/1000u : UINT32_MAX; }
static void net_init(void){
    esp_netif_init(); esp_event_loop_create_default();
    wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&c);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start();
    esp_wifi_set_channel(ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
    esp_now_init();
    esp_now_peer_info_t p={0}; memcpy(p.peer_addr,BCAST,6); p.channel=ESPNOW_CH; p.ifidx=WIFI_IF_STA;
    esp_now_add_peer(&p); esp_now_register_recv_cb(on_recv);
    esp_fill_random(s_salt,4);
    ESP_LOGW(TAG, "espnow up (ch=%d), requesting...", ESPNOW_CH);
}

// ---- Touch: XPT2046 press-detect (T_IRQ, active-LOW on contact; externally pulled high when
// idle) plus bit-banged coordinate reads. No free SPI host (SPI2=display, SPI3=SD), so the
// controller is clocked by hand on its dedicated CYD pins. Coarse calibration -> zone taps. ----
static void touch_init(void){
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TOUCH_IRQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // GPIO34-39 are input-only, no internal pulls anyway
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_config_t oc = { .pin_bit_mask = (1ULL<<TOUCH_CLK_GPIO)|(1ULL<<TOUCH_CS_GPIO)|(1ULL<<TOUCH_DIN_GPIO),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&oc);
    gpio_config_t ic = { .pin_bit_mask = 1ULL<<TOUCH_DOUT_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&ic);
    gpio_set_level(TOUCH_CS_GPIO, 1);   // idle high
}

// Bit-banged XPT2046 read (SPI mode 0). cmd 0x90 = X, 0xD0 = Y (12-bit, single-ended).
static uint16_t xpt_xfer(uint8_t cmd)
{
    for (int i = 7; i >= 0; i--) {                       // send command MSB-first
        gpio_set_level(TOUCH_DIN_GPIO, (cmd >> i) & 1);
        gpio_set_level(TOUCH_CLK_GPIO, 1); gpio_set_level(TOUCH_CLK_GPIO, 0);
    }
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {                        // read 16 clocks, take top 12 bits
        gpio_set_level(TOUCH_CLK_GPIO, 1);
        v = (uint16_t)((v << 1) | (gpio_get_level(TOUCH_DOUT_GPIO) & 1));
        gpio_set_level(TOUCH_CLK_GPIO, 0);
    }
    return v >> 4;
}

static bool touch_read_xy(int *x, int *y)
{
    if (gpio_get_level(TOUCH_IRQ_GPIO)) return false;    // not pressed (idle high)
    gpio_set_level(TOUCH_CS_GPIO, 0);
    uint16_t rx = xpt_xfer(0x90);
    uint16_t ry = xpt_xfer(0xD0);
    gpio_set_level(TOUCH_CS_GPIO, 1);
    if (rx < 60 || ry < 60) return false;                // reject noise/no-contact
    // Panel-measured calibration (4-corner tap). On this CYD the XPT2046 axes are SWAPPED and
    // rawY is INVERTED relative to the ILI9341 portrait frame: raw X (~[110..1840]) tracks
    // screen Y top->bottom; raw Y (~[175..1925]) tracks screen X right->left.
    #define TCAL_X_MIN 110
    #define TCAL_X_MAX 1840
    #define TCAL_Y_MIN 175
    #define TCAL_Y_MAX 1925
    int px = (int)((TCAL_Y_MAX - (int)ry) * LCD_W / (TCAL_Y_MAX - TCAL_Y_MIN));   // rawY -> screen X (inverted)
    int py = (int)(((int)rx - TCAL_X_MIN) * LCD_H / (TCAL_X_MAX - TCAL_X_MIN));   // rawX -> screen Y
    if (px < 0) px = 0;
    if (px >= LCD_W) px = LCD_W - 1;
    if (py < 0) py = 0;
    if (py >= LCD_H) py = LCD_H - 1;
    *x = px; *y = py;
    return true;
}
// CYD-side freshness overlay: drawn as one extra band over the top of the just-rendered view,
// so the shared renderer stays untouched. Not shown while data is fresh (<=15s old).
// The window is wide relative to the ~1s request cadence so an occasional lost
// ESP-NOW broadcast (BLE+Wi-Fi coexist) doesn't flash a spurious "NO DECOY".
static void draw_freshness_overlay(uint16_t *band, uint32_t now){
    // s_status_ms is stamped by the ESP-NOW recv callback (a separate, higher-priority task) and
    // can be a few ms AHEAD of this loop's cached `now` — the reply to the request we just sent
    // lands before we draw. A plain unsigned (now - s_status_ms) then underflows to ~4.29e9 and
    // paints a permanent spurious "NO DECOY". Compare signed so any not-in-the-past sample is fresh.
    if (s_status_ms != 0 && (int32_t)(now - s_status_ms) <= 15000) return;
    radar_gfx_t g = { .buf = band, .w = LCD_W, .y0 = 0, .h = 40 };
    radar_gfx_clear(&g, 0x0000);
    if (s_status_ms == 0) radar_gfx_text(&g, 56, 16, "SEARCHING...", 0xFFFF);
    else                  radar_gfx_text(&g, 68, 16, "NO DECOY", 0x7BEF);
    cyd_flush(0, 40, band, NULL);
}

void app_main(void)
{
    nvs_flash_init();
    if (!cyd_panel_init(&s_panel)) { ESP_LOGE(TAG, "panel init failed"); return; }
    touch_init();
    net_init();

    s_sd_ok = sd_mount();
    if (s_sd_ok) {                                   // one-shot probe: mkdir + write + read-back
        mkdir(SD_MOUNT_POINT "/simulacra", 0777);
        FILE *f = fopen(SD_MOUNT_POINT "/simulacra/probe.txt", "w");
        if (f) { fputs("ok", f); fclose(f); ESP_LOGW(TAG, "sd: probe write ok"); }
        else ESP_LOGW(TAG, "sd: probe write FAILED");
    }
    learn_db_load();
    sig_db_init();

    radar_ui_t ui; radar_ui_reset(&ui, (uint32_t)(esp_timer_get_time()/1000), 0);
    static uint16_t band[LCD_W*40]; uint16_t sweep=0; uint32_t last_req=0;
    bool bl_was_on = true;
    ESP_LOGW(TAG, "panel up: live radar loop starting");
    for(;;){
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        int tx, ty;
        bool press = touch_read_xy(&tx, &ty);
        static bool was_press = false;
        bool edge = press && !was_press;                 // fresh contact
        was_press = press;
        if (edge) {
            if (ui.view == RADAR_VIEW_CONTROL) {
                radar_ui_note_input(&ui, now);           // keep backlight/idle timer fresh
#ifdef SIMULACRA_CONFIG_CTRL
                if (ty > 200 && tx > 60 && tx < 180) {   // SEND button
                    send_config(ui.sel_preset);
                    radar_ctrl_mark_sent(&ui, now);
                } else if (tx < 80) {                    // left zone: prev == cycle-around
                    for (int i = 0; i < RADAR_CTRL_PRESET_COUNT - 1; i++) radar_ctrl_select_next(&ui);
                } else if (tx > 160) {                   // right zone: next
                    radar_ctrl_select_next(&ui);
                } else {                                 // center tap = leave to next view
                    radar_ui_on_input(&ui, now); send_request(); last_req = now;
                }
#else
                radar_ui_on_input(&ui, now); send_request(); last_req = now;
#endif
            } else {
                radar_ui_on_input(&ui, now); send_request(); last_req = now;
            }
        }
        // keep asking every ~1s while the screen is awake so data stays fresh
        if (ui.backlight_on && now-last_req > 1000) { send_request(); last_req=now; }
        static uint32_t last_sync = 0;
        if (now - last_sync > 20000) { last_sync = now; broadcast_library(); }   // every 20 s
        static uint32_t last_sig = 0;
        if (now - last_sig > 60000) { last_sig = now; broadcast_sig_db(); }      // signature DB every 60 s
        static uint32_t last_save = 0;
        if (s_lib_dirty && now - last_save > LEARN_DB_SAVE_MS) {
            last_save = now; s_lib_dirty = false; learn_db_save();
        }
        radar_ui_on_tick(&ui, now, s_status.threat_count);
        if (ui.backlight_on != bl_was_on) {
            set_backlight(ui.backlight_on);
            // On wake, grant a freshness grace: while asleep no requests go out so s_status_ms is
            // frozen stale, which would paint a spurious "NO DECOY" for ~1s until the first
            // post-wake reply. Only if we'd already seen a decoy; a never-seen decoy keeps
            // "SEARCHING...", and a truly-gone decoy still expires the 15s window honestly.
            if (ui.backlight_on && s_status_ms != 0) s_status_ms = now;
            bl_was_on = ui.backlight_on;
        }
        if (ui.backlight_on){
            radar_lib_info_t lib = {
                .sd_ok = s_sd_ok,
                .card_mb = s_sd_ok ? (uint32_t)(((uint64_t)s_card->csd.capacity)*s_card->csd.sector_size/(1024*1024)) : 0,
                .lib_count = (uint16_t)s_lib_count, .lib_cap = VIGIL_LIB_CAP,
                .offer_age_s = age_s(now, s_last_offer_ms),
                .sync_age_s  = age_s(now, s_last_sync_ms),
                .save_age_s  = age_s(now, s_last_save_ms),
                .save_bytes  = s_save_bytes,
            };
            radar_ctrl_info_t ctrl = { .sel_preset = ui.sel_preset,
                .send_flash = (ui.send_flash_ms && (now - ui.send_flash_ms) < RADAR_CTRL_FLASH_MS) };
            radar_render_view(ui.view, &s_status, &lib, &ctrl, sweep, band, 40, LCD_W, LCD_H, cyd_flush, NULL);
            draw_freshness_overlay(band, now);
            sweep=(uint16_t)((sweep+12)%360);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
