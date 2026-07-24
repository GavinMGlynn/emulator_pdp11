#ifndef PDP11_CACHE_H
#define PDP11_CACHE_H

#include <stdbool.h>
#include <stdint.h>

// PDP-11/70 cache (KB11-C Processor Manual sec. 2.2). Two-way set-associative:
// two groups of 256 blocks, each block two words = 1K words total. Address split
// of the 22-bit physical address: tag = bits 21-10, index/set = bits 9-2, word =
// bit 1, byte = bit 0. Write-through, and hardware uses *random* replacement —
// which is non-deterministic, so this model uses a per-set round-robin victim as
// a documented deterministic stand-in (the hit/miss result is insensitive to the
// policy for localized access, which is the common case). See FINDINGS.
//
// This structure is timing-only: it holds valid bits and tags to count read
// misses (each adds 1.02 us to instruction time); the data always comes from
// main memory in the functional model, so it needs no data array.
#define PDP11_CACHE_SETS 256

typedef struct pdp11_cache {
    uint16_t tag[PDP11_CACHE_SETS][2];
    bool valid[PDP11_CACHE_SETS][2];
    uint8_t victim[PDP11_CACHE_SETS]; // next way to replace on a miss
    uint64_t misses;                  // cumulative read misses
} pdp11_cache;

// Invalidate every line and clear the miss counter.
void pdp11_cache_reset(pdp11_cache *c);

// Model a read of physical address pa: returns true on a hit; on a miss it
// allocates the block (round-robin victim) and increments the miss counter.
bool pdp11_cache_read(pdp11_cache *c, uint32_t pa);

#endif // PDP11_CACHE_H
