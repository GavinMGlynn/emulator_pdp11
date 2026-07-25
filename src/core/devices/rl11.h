#ifndef PDP11_RL11_H
#define PDP11_RL11_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// RL11 controller / RL01-RL02 disk. A Unibus/Q-bus DMA disk: four registers on
// the I/O page (0174400-0174406, plus RLBAE at 0174410 for 22-bit), interrupting
// at BR5 through vector 0160. The controller transfers whole sectors between
// memory (18/22-bit RLBA[E]) and the disk (cylinder/head/sector in RLDA),
// counting the two's-complement word count in RLMP. Register and command
// semantics mirror SimH pdp11_rl.c.
//
// The disk image is supplied by the frontend as a word buffer via pdp11_rl_attach
// (drive 0). Seek/rotational delay is modelled as a completion delay in emulated
// time, after which DONE is set and, if enabled, the interrupt requested; the
// data transfer itself is functional.

#define RL_CSR   0174400u // register base (RLCS)
#define RL_VEC   0000160u // interrupt vector
#define RL_IPL   5        // bus request level (BR5)

#define RL_RLCS  0174400u // control/status
#define RL_RLBA  0174402u // bus (memory) address
#define RL_RLDA  0174404u // disk address
#define RL_RLMP  0174406u // multipurpose register (word count / status / header)
#define RL_RLBAE 0174410u // bus address extension (Qbus RLV12 only; NXM on Unibus)
#define RL_END   0174406u  // last register the Unibus RL11 decodes (RLMP)

#define RL_NUMWD 128u  // words per sector
#define RL_NUMSC 40u   // sectors per surface
#define RL_NUMSF 2u    // surfaces per cylinder
#define RL_NUMCY 256u  // cylinders per drive (RL01; RL02 doubles this)
#define RL01_WORDS (RL_NUMCY * RL_NUMSF * RL_NUMSC * RL_NUMWD)     // words, RL01
#define RL02_WORDS (RL01_WORDS * 2u)                              // words, RL02

// RL11 controller state (embedded in the CPU). A single drive 0.
typedef struct {
    uint16_t rlcs, rlba, rlda, rlmp, rlbae;
    uint16_t trk;        // the drive's current head position (RLDA read format)
    uint16_t *disk;      // drive 0 backing store (words), or NULL if detached
    uint32_t disk_words; // its size in words (selects RL01 vs RL02)
    bool busy;           // a transfer is scheduled and not yet complete
    uint64_t done_ns;    // emulated time the current transfer completes
} pdp11_rl11;

// Register access, dispatched from the CPU's I/O-page decode.
uint16_t pdp11_rl_read(pdp11_cpu *cpu, uint16_t addr);
void pdp11_rl_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value);

// Attach drive 0's backing store (a buffer of `words` 16-bit words; RL01_WORDS
// or RL02_WORDS). Passing NULL detaches. The buffer is caller-owned.
void pdp11_rl_attach(pdp11_cpu *cpu, uint16_t *disk, uint32_t words);

// Complete a scheduled transfer once its emulated-time delay has elapsed.
void pdp11_rl_poll(pdp11_cpu *cpu);

#endif // PDP11_RL11_H
