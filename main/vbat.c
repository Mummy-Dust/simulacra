#include "vbat.h"

/* ---- Backend 1: MAX17048/MAX17049 fuel gauge over I2C (SparkFun Thing Plus C6) ---------------- */
#if defined(SIMULACRA_VBAT_MAX17048)
#include "driver/i2c_master.h"
#include "esp_log.h"
#ifndef SIMULACRA_VBAT_LOW_PCT
#define SIMULACRA_VBAT_LOW_PCT 15
#endif
#if !defined(SIMULACRA_VBAT_SDA) || !defined(SIMULACRA_VBAT_SCL)
#error "SIMULACRA_VBAT_MAX17048=1 requires -DSIMULACRA_VBAT_SDA=<gpio> and -DSIMULACRA_VBAT_SCL=<gpio>"
#endif
#define MAX17048_ADDR 0x36
#define REG_VCELL     0x02   // 16-bit, 78.125 uV/LSB
#define REG_SOC       0x04   // 16-bit, 1/256 %/LSB

static const char *VTAG = "vbat";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int  s_mv = -1, s_soc = -1;

static int rd_reg(uint8_t reg, uint16_t *out)
{
    uint8_t r = reg, b[2];
    if (i2c_master_transmit_receive(s_dev, &r, 1, b, 2, 100) != ESP_OK) return -1;
    *out = (uint16_t)((b[0] << 8) | b[1]);   // MAX17048 is big-endian
    return 0;
}
static void sample(void)
{
    uint16_t v, s;
    if (rd_reg(REG_VCELL, &v) == 0) s_mv  = (int)((uint32_t)v * 5 / 64);   // 78.125 uV = 5/64 mV
    if (rd_reg(REG_SOC,   &s) == 0) s_soc = (int)(s / 256);
}
void vbat_init(void)
{
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = -1,
        .sda_io_num = SIMULACRA_VBAT_SDA, .scl_io_num = SIMULACRA_VBAT_SCL,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGW(VTAG, "i2c bus init failed"); return; }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = MAX17048_ADDR, .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) { ESP_LOGW(VTAG, "i2c add dev failed"); return; }
    uint16_t probe;
    if (rd_reg(REG_VCELL, &probe) == 0) { s_present = true; sample();
        ESP_LOGW(VTAG, "MAX17048 present: %d mV, %d%%", s_mv, s_soc); }
    else ESP_LOGW(VTAG, "no fuel gauge on I2C (SDA=%d SCL=%d)", SIMULACRA_VBAT_SDA, SIMULACRA_VBAT_SCL);
}
bool vbat_present(void) { return s_present; }
int  vbat_mv(void)      { if (s_present) sample(); return s_present ? s_mv  : -1; }
int  vbat_soc_pct(void) { if (s_present) sample(); return s_present ? s_soc : -1; }
bool vbat_low(void)     { return s_present && vbat_soc_pct() >= 0 && s_soc < SIMULACRA_VBAT_LOW_PCT; }

/* ---- Backend 2: ADC voltage divider (Waveshare ESP32-C5-WIFI6-KIT: BAT_ADC on GPIO6, /3) ------ */
#elif defined(SIMULACRA_VBAT_ADC)
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#ifndef SIMULACRA_VBAT_ADC_GPIO
#error "SIMULACRA_VBAT_ADC=1 requires -DSIMULACRA_VBAT_ADC_GPIO=<gpio>"
#endif
#ifndef SIMULACRA_VBAT_ADC_DIV
#define SIMULACRA_VBAT_ADC_DIV 3          // 200k/100k divider -> Vbat = Vadc * 3
#endif
#ifndef SIMULACRA_VBAT_LOW_MV
#define SIMULACRA_VBAT_LOW_MV 3400
#endif
#define VBAT_PRESENT_MV 2500              // below this = no cell / floating, not a real battery

static const char *VTAG = "vbat";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static adc_unit_t s_unit;
static bool s_ready;

void vbat_init(void)
{
    if (adc_oneshot_io_to_channel(SIMULACRA_VBAT_ADC_GPIO, &s_unit, &s_chan) != ESP_OK) {
        ESP_LOGW(VTAG, "gpio %d is not ADC-capable", SIMULACRA_VBAT_ADC_GPIO); return; }
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = s_unit };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { ESP_LOGW(VTAG, "adc unit init failed"); return; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, s_chan, &cc);
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = s_unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK)
        ESP_LOGW(VTAG, "adc calibration unavailable (using raw scaling)");
    s_ready = true;
    ESP_LOGW(VTAG, "battery ADC gpio %d (div %d): %d mV", SIMULACRA_VBAT_ADC_GPIO, SIMULACRA_VBAT_ADC_DIV, vbat_mv());
}
int vbat_mv(void)
{
    if (!s_ready) return -1;
    int raw; if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) return -1;
    int mv;
    if (s_cali) { if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1; }
    else mv = raw * 3300 / 4095;                     // crude fallback: 12-bit, ~3.3V ref
    return mv * SIMULACRA_VBAT_ADC_DIV;
}
int  vbat_soc_pct(void) { return -1; }               // an ADC divider gives voltage, not SoC
bool vbat_present(void) { return vbat_mv() > VBAT_PRESENT_MV; }
bool vbat_low(void)     { int mv = vbat_mv(); return mv > VBAT_PRESENT_MV && mv < SIMULACRA_VBAT_LOW_MV; }

/* ---- Disabled: no battery hardware, zero cost --------------------------------------------------- */
#else
void vbat_init(void)    {}
bool vbat_present(void) { return false; }
int  vbat_mv(void)      { return -1; }
int  vbat_soc_pct(void) { return -1; }
bool vbat_low(void)     { return false; }
#endif
