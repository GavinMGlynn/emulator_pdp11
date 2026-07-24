#include "memory/memory.h"

#include <string.h>

void pdp11_mem_reset(pdp11_mem *m) {
    memset(m->words, 0, sizeof m->words);
}

uint16_t pdp11_mem_read_word(const pdp11_mem *m, uint32_t addr) {
    return m->words[(addr & (PDP11_MEM_BYTES - 1u)) >> 1];
}

void pdp11_mem_write_word(pdp11_mem *m, uint32_t addr, uint16_t value) {
    m->words[(addr & (PDP11_MEM_BYTES - 1u)) >> 1] = value;
}

uint8_t pdp11_mem_read_byte(const pdp11_mem *m, uint32_t addr) {
    uint16_t word = pdp11_mem_read_word(m, addr);
    return (uint8_t)((addr & 1u) ? (word >> 8) : (word & 0xFFu));
}

void pdp11_mem_write_byte(pdp11_mem *m, uint32_t addr, uint8_t value) {
    uint32_t windex = (addr & (PDP11_MEM_BYTES - 1u)) >> 1;
    uint16_t word = m->words[windex];
    if (addr & 1u) {
        word = (uint16_t)((word & 0x00FFu) | ((uint32_t)value << 8));
    } else {
        word = (uint16_t)((word & 0xFF00u) | value);
    }
    m->words[windex] = word;
}
