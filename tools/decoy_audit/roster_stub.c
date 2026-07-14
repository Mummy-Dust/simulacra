/* Host stub: roster_init() references rf_model_load_nvs(), but synth_dump builds the roster from
   a model-seed file (or the fallback model), never NVS. The learn_* API now comes from the REAL
   main/learn.c (so the audit can measure self-learned shapes); only this one symbol still needs a
   host stub. No NVS on the host -> "no model". */
#include "rf_model.h"
int rf_model_load_nvs(rf_model_t *m) { (void)m; return 1; }
