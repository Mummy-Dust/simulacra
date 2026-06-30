#pragma once
#include <stdbool.h>
#include "rf_model.h"

// Drift between two models, 0..~1 (0 = identical environment). Vendor-mix L1
// distance (normalized to 0..1) blended with the normalized population delta.
// Pure: no radio. Shared by live re-profiling (Layer 2) and anti-entourage (Layer 3).
float drift_score(const rf_model_t *prev, const rf_model_t *cur);
// True when score is strictly above threshold.
bool  drift_exceeds(float score, float threshold);
