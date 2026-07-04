#include <string.h>
#include "sig_wire.h"

int sig_wire_pack(uint8_t *payload, size_t *plen, const threat_sig_t *recs, uint8_t nrecs,
                  uint16_t content_version, uint8_t chunk_index, uint8_t chunk_count)
{
    if (nrecs > SIG_WIRE_RECS_PER_CHUNK) return -1;
    sig_chunk_hdr_t h = { content_version, chunk_index, chunk_count, nrecs };
    memcpy(payload, &h, sizeof h);
    memcpy(payload + sizeof h, recs, (size_t)nrecs * sizeof(threat_sig_t));
    *plen = sizeof h + (size_t)nrecs * sizeof(threat_sig_t);
    return 0;
}

int sig_wire_unpack(const uint8_t *payload, size_t plen, threat_sig_t *recs,
                    uint8_t *nrecs, sig_chunk_hdr_t *hdr)
{
    if (plen < sizeof(sig_chunk_hdr_t)) return -1;
    memcpy(hdr, payload, sizeof *hdr);
    if (hdr->rec_count > SIG_WIRE_RECS_PER_CHUNK) return -1;
    size_t need = sizeof(sig_chunk_hdr_t) + (size_t)hdr->rec_count * sizeof(threat_sig_t);
    if (plen != need) return -1;
    memcpy(recs, payload + sizeof(sig_chunk_hdr_t), (size_t)hdr->rec_count * sizeof(threat_sig_t));
    *nrecs = hdr->rec_count;
    return 0;
}
