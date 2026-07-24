#ifndef PDP11_MEMORY_H
#define PDP11_MEMORY_H

#include <stdint.h>

// Physical memory.
//
// The PDP-11 is byte-addressable with 16-bit words in little-endian order
// (the low byte lives at the even address). The 11/70 supports 22-bit physical
// addressing — up to 2 Miwords / 4 MiB — so we size the backing store to the
// full space. Address decoding for the Unibus I/O page and MMU relocation land
// in later phases (see docs/COMPLETION_PLAN.md); for now this is flat RAM.
#define PDP11_MEM_WORDS (1u << 21) // 2 Miwords = 4 MiB (22-bit physical space)
#define PDP11_MEM_BYTES (PDP11_MEM_WORDS * 2u)

typedef struct pdp11_mem {
    uint16_t words[PDP11_MEM_WORDS];
} pdp11_mem;

// Zero all of memory.
void pdp11_mem_reset(pdp11_mem *m);

// Word access. `addr` is a byte address and must be even; the low bit is
// ignored (the hardware forces word alignment on the internal data path).
uint16_t pdp11_mem_read_word(const pdp11_mem *m, uint32_t addr);
void pdp11_mem_write_word(pdp11_mem *m, uint32_t addr, uint16_t value);

// Byte access (little-endian: even address = low byte, odd = high byte).
uint8_t pdp11_mem_read_byte(const pdp11_mem *m, uint32_t addr);
void pdp11_mem_write_byte(pdp11_mem *m, uint32_t addr, uint8_t value);

#endif // PDP11_MEMORY_H
