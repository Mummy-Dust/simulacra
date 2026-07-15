#pragma once
#include <stdint.h>
#include <stdbool.h>

// Battery telemetry via a MAX17048/MAX17049-family fuel gauge on I2C (addr 0x36) -- the chip on
// the SparkFun Thing Plus C6, and on a Qwiic MAX17048 breakout you can add to any board.
//
// Opt-in and board-specific. Enable with the build flags:
//   -DSIMULACRA_VBAT=1  -DSIMULACRA_VBAT_SDA=<gpio>  -DSIMULACRA_VBAT_SCL=<gpio>
//   [-DSIMULACRA_VBAT_LOW_PCT=15]   (state-of-charge %% below which LOW BATT is flagged)
// Disabled (the default) -> every call is a no-op returning "absent", zero cost, no I2C bus.

void vbat_init(void);        // probe the gauge once at boot; no-op if disabled or none present
bool vbat_present(void);     // true iff a gauge answered on I2C
int  vbat_mv(void);          // fresh cell voltage in mV, or -1 if absent
int  vbat_soc_pct(void);     // fresh state-of-charge %, or -1 if absent
bool vbat_low(void);         // present AND soc < SIMULACRA_VBAT_LOW_PCT (early brownout warning)
