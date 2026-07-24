#ifndef PDP11_RK11_H
#define PDP11_RK11_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// RK11 controller / RK05 disk (P6). A Unibus DMA disk: eight registers on the
// I/O page (0177400-0177416), interrupting at BR5 through vector 0220. The
// controller transfers whole sectors between memory (18-bit Unibus address in
// RKBA + RKCS<5:4>) and the disk (block RKDA), counting RKWC words. Register and
// command semantics mirror SimH pdp11_rk.c.
//
// The disk image is supplied by the frontend as a word buffer via
// pdp11_rk_attach (drive 0). Timing (seek + rotational + transfer) is modelled
// as a completion delay in emulated time, after which DONE is set and, if
// enabled, the interrupt requested; the data transfer itself is functional.

#define RK_CSR    0177400u // register base (RKDS)
#define RK_VEC    0000220u // interrupt vector
#define RK_IPL    5        // bus request level (BR5)

#define RK_RKDS 0177400u // drive status  (read only)
#define RK_RKER 0177402u // error status  (read only)
#define RK_RKCS 0177404u // control/status
#define RK_RKWC 0177406u // word count (two's complement)
#define RK_RKBA 0177410u // memory (bus) address
#define RK_RKDA 0177412u // disk address
#define RK_RKMR 0177414u // maintenance (unimplemented)
#define RK_RKDB 0177416u // data buffer (unimplemented)

#define RK_NUMWD  256u // words per sector
#define RK_NUMSC  12u  // sectors per surface
#define RK_NUMSF  2u   // surfaces per cylinder
#define RK_NUMCY  203u // cylinders per drive
#define RK_WORDS  (RK_NUMCY * RK_NUMSF * RK_NUMSC * RK_NUMWD) // words per drive

// RK11 controller state (embedded in the CPU).
typedef struct {
    uint16_t rkcs, rker, rkwc, rkba, rkda, rkds;
    uint16_t *disk;      // drive 0 backing store (words), or NULL if detached
    uint32_t disk_words; // its size in words
    bool busy;           // a transfer is scheduled and not yet complete
    uint64_t done_ns;    // emulated time the current transfer completes
} pdp11_rk11;

// Register access, dispatched from the CPU's I/O-page decode.
uint16_t pdp11_rk_read(pdp11_cpu *cpu, uint16_t addr);
void pdp11_rk_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value);

// Attach drive 0's backing store (a buffer of `words` 16-bit words). Passing
// NULL detaches. The buffer is owned by the caller and must outlive the CPU.
void pdp11_rk_attach(pdp11_cpu *cpu, uint16_t *disk, uint32_t words);

// Complete a scheduled transfer once its emulated-time delay has elapsed.
void pdp11_rk_poll(pdp11_cpu *cpu);

#endif // PDP11_RK11_H
