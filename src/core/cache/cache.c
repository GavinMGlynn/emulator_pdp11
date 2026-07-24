#include "cache/cache.h"

#include <string.h>

void pdp11_cache_reset(pdp11_cache *c) {
    memset(c, 0, sizeof *c);
}

bool pdp11_cache_read(pdp11_cache *c, uint32_t pa) {
    int set = (int)((pa >> 2) & 0377u);       // index field, bits 9-2
    uint16_t tag = (uint16_t)((pa >> 10) & 07777u); // address field, bits 21-10

    if ((c->valid[set][0] && c->tag[set][0] == tag) ||
        (c->valid[set][1] && c->tag[set][1] == tag)) {
        return true; // hit
    }

    // Miss: allocate the block into the round-robin victim way.
    uint8_t w = c->victim[set];
    c->tag[set][w] = tag;
    c->valid[set][w] = true;
    c->victim[set] = (uint8_t)(w ^ 1u);
    c->misses++;
    return false;
}
