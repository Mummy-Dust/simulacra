#include "vbat.h"

#ifndef SIMULACRA_VBAT
#define SIMULACRA_VBAT 0
#endif
#ifndef SIMULACRA_VBAT_LOW_PCT
#define SIMULACRA_VBAT_LOW_PCT 15
#endif

#if SIMULACRA_VBAT
#include "driver/i2c_master.h"
#include "esp_log.h"

#if !defined(SIMULACRA_VBAT_SDA) || !defined(SIMULACRA_VBAT_SCL)
#error "SIMULACRA_VBAT=1 requires -DSIMULACRA_VBAT_SDA=<gpio> and -DSIMULACRA_VBAT_SCL=<gpio>"
#endif

#define MAX17048_ADDR 0x36
#define REG_VCELL     0x02   // 16-bit, 78.125 uV/LSB
#define REG_SOC       0x04   // 16-bit, 1/256 %/LSB (high byte ~= integer %)

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
    if (rd_reg(REG_VCELL, &probe) == 0) {
        s_present = true; sample();
        ESP_LOGW(VTAG, "MAX17048 present: %d mV, %d%%", s_mv, s_soc);
    } else {
        ESP_LOGW(VTAG, "no fuel gauge on I2C (SDA=%d SCL=%d)", SIMULACRA_VBAT_SDA, SIMULACRA_VBAT_SCL);
    }
}

bool vbat_present(void) { return s_present; }
int  vbat_mv(void)      { if (s_present) sample(); return s_present ? s_mv  : -1; }
int  vbat_soc_pct(void) { if (s_present) sample(); return s_present ? s_soc : -1; }
bool vbat_low(void)     { return s_present && vbat_soc_pct() >= 0 && s_soc < SIMULACRA_VBAT_LOW_PCT; }

#else  /* SIMULACRA_VBAT disabled: no gauge, zero cost. */
void vbat_init(void)    {}
bool vbat_present(void) { return false; }
int  vbat_mv(void)      { return -1; }
int  vbat_soc_pct(void) { return -1; }
bool vbat_low(void)     { return false; }
#endif
