#ifndef PDP11_RP11_H
#define PDP11_RP11_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// RH70 Massbus adapter + RP04 disk (P6). The 11/70's high-speed disk path: the
// controller registers sit on the I/O page at 0176700-0176752, interrupting at
// BR5 through vector 0254. The RH70 owns CS1/WC/BA/CS2/BAE and passes the other
// register offsets to the drive (DS/ER1/DA/DT/DC/CC/...); the I/O-offset ->
// register mapping is SimH's mba_mapofs. A read/write transfers whole sectors
// between memory (22-bit BAE:BA) and the disk block GET_DA(DC,DA), counting WC
// words. Register/command semantics mirror SimH pdp11_rh.c + pdp11_rp.c; the
// data transfer is functional and completion is scheduled in emulated time.
//
// This is a single RP04 on drive 0, sufficient for the boot path; the full
// Massbus error/attention/diagnostic model is a documented tail.

#define RP_CSR 0176700u // register window base (RPCS1)
#define RP_END 0176752u // last register (RPCS3)
#define RP_VEC 0000254u // interrupt vector
#define RP_IPL 5        // bus request level (BR5)

#define RP_NUMWD 256u  // words per sector
#define RP_SECT  22u   // sectors per track (RP04)
#define RP_SURF  19u   // tracks per cylinder (RP04)
#define RP_CYL   411u  // cylinders (RP04)
#define RP_WORDS (RP_SECT * RP_SURF * RP_CYL * RP_NUMWD) // words per RP04 drive

// RH70 + RP04 controller state (embedded in the CPU).
typedef struct {
    uint16_t cs1, wc, ba, cs2, bae;      // RH70 registers
    uint16_t da, dc, cc, ds, er1;        // RP04 drive registers
    uint16_t *disk;                      // drive-0 backing store (words) or NULL
    uint32_t disk_words;
    bool busy;
    uint64_t done_ns;
} pdp11_rp11;

// Register access, dispatched from the CPU's I/O-page decode.
uint16_t pdp11_rp_read(pdp11_cpu *cpu, uint16_t addr);
void pdp11_rp_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value);

// Attach drive 0's backing store (a buffer of `words` 16-bit words), or NULL to
// detach. The buffer is owned by the caller and must outlive the CPU.
void pdp11_rp_attach(pdp11_cpu *cpu, uint16_t *disk, uint32_t words);

// Complete a scheduled transfer once its emulated-time delay has elapsed.
void pdp11_rp_poll(pdp11_cpu *cpu);

#endif // PDP11_RP11_H
