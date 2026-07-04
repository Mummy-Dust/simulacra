#pragma once
#include "esp_log.h"

// Compile-gated behavioral trace. Default OFF: SIM_TRACE compiles to nothing, so shipping builds
// stay quiet and pay zero cost. Build with `idf.py -DSIMULACRA_TRACE=1` to follow the decoy's
// decisions (model load, roster/crowd composition, churn transitions) when chasing a bug.
#ifndef SIMULACRA_TRACE
#define SIMULACRA_TRACE 0
#endif

#if SIMULACRA_TRACE
#define SIM_TRACE(tag, fmt, ...) ESP_LOGW(tag, "[trace] " fmt, ##__VA_ARGS__)
#else
#define SIM_TRACE(tag, fmt, ...) do { (void)0; } while (0)
#endif
