#pragma once
#include "learn_record.h"
#include "law3.h"

// Idempotent merge of one record into a (store,count,cap) library, keyed by shape_hash.
// Reinforce if present (saturating count, refresh last_seen, widen interval band); else
// insert, evicting the weakest when full. Returns false iff full and rec is the weakest.
bool learn_merge(learned_template_t *store, size_t *count, size_t cap,
                 const learned_template_t *rec, uint16_t sweep_no);
