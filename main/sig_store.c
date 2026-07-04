#include "sig_store.h"
#include "sig_match.h"      // sig_regate
#include <string.h>

static threat_sig_t s_db[SIG_DB_CAP];
static size_t       s_count;
static uint16_t     s_version;

void sig_store_load_seed(void)
{
    s_count = sig_seed_copy(s_db, SIG_DB_CAP);
    s_version = sig_seed_version();
}
size_t sig_store_count(void)              { return s_count; }
const threat_sig_t *sig_store_db(void)    { return s_db; }
uint16_t sig_store_version(void)          { return s_version; }

bool sig_store_adopt(const threat_sig_t *recs, size_t n, uint16_t content_version)
{
    if (content_version < s_version) return false;      // newest-wins
    threat_sig_t tmp[SIG_DB_CAP]; size_t m = 0;
    for (size_t i = 0; i < n && m < SIG_DB_CAP; i++)
        if (sig_regate(&recs[i])) tmp[m++] = recs[i];   // never trust the wire
    if (m == 0) return false;
    memcpy(s_db, tmp, m * sizeof(threat_sig_t));
    s_count = m; s_version = content_version;
    return true;
}
