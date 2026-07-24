#ifndef PDP11_TM11_H
#define PDP11_TM11_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// TM11 / TU10 magnetic tape (P6). A Unibus record-oriented DMA tape controller:
// six registers on the I/O page (0172520-0172532), interrupting at BR5 through
// vector 0224. The tape image is the SimH ".tap" format — each record is a
// 4-byte little-endian length, that many data bytes (padded to even), and a
// trailing length; a length of 0 is a tape mark (file mark). Read/write move a
// whole record between memory (18-bit MTCMA + MTC<5:4>) and the tape, counting
// down MTBRC bytes. Register/command semantics mirror SimH pdp11_tm.c.
//
// A single TU10 on drive 0, sufficient for install media; multi-drive and the
// 7-track / density / parity details are documented tails.

#define TM_CSR 0172520u // register base (MTS)
#define TM_END 0172532u // last register (MTRD)
#define TM_VEC 0000224u // interrupt vector
#define TM_IPL 5        // bus request level (BR5)

#define TM_MTS  0172520u // status (read only)
#define TM_MTC  0172522u // command
#define TM_MTBRC 0172524u // byte/record count
#define TM_MTCMA 0172526u // current memory address
#define TM_MTD  0172530u // data buffer
#define TM_MTRD 0172532u // read lines (read only)

// TM11 controller state (embedded in the CPU).
typedef struct {
    uint16_t sta, cmd, bc, ca, db, rdl;
    uint8_t *tape;       // .tap image (bytes), or NULL if detached
    uint32_t tape_len;
    uint32_t pos;        // current byte offset in the image
    bool wrp;            // write protected
    bool busy;
    uint64_t done_ns;
} pdp11_tm11;

// Register access, dispatched from the CPU's I/O-page decode.
uint16_t pdp11_tm_read(pdp11_cpu *cpu, uint16_t addr);
void pdp11_tm_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value);

// Attach drive 0's .tap image (`len` bytes), or NULL to detach. `wrp` write-
// protects it. The buffer is owned by the caller and must outlive the CPU.
void pdp11_tm_attach(pdp11_cpu *cpu, uint8_t *tape, uint32_t len, bool wrp);

// Complete a scheduled tape operation once its emulated-time delay has elapsed.
void pdp11_tm_poll(pdp11_cpu *cpu);

#endif // PDP11_TM11_H
