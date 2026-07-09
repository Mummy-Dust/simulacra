/* Host stubs for symbols the generator links against but the audit tool disables:
   - learn_*: the self-learning path is off (learn_count()==0 => generate.c never
     samples a learned template), so the audit measures the built-in generator.
   - rf_model_load_nvs: roster_init() references it; synth_dump never calls
     roster_init, but the symbol must resolve. No NVS on the host -> "no model". */
#include "learn.h"
#include "rf_model.h"
size_t                    learn_count(void) { return 0; }
const learned_template_t *learn_at(size_t i) { (void)i; return 0; }
int                       learn_render(const learned_template_t *lt, uint8_t out[31],
                                       uint8_t *len, uint16_t *itvl)
{ (void)lt; (void)out; (void)len; (void)itvl; return 1; }
int rf_model_load_nvs(rf_model_t *m) { (void)m; return 1; }
