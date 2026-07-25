#ifndef PDP11_RX11_H
#define PDP11_RX11_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// RX11 controller / RX01 8-inch floppy. A programmed-I/O device (no DMA): two
// registers on the I/O page (RXCS 0177170, RXDB 0177172), interrupting at BR5
// through vector 0264. Data moves a byte at a time through RXDB under a
// transfer-request (TR) handshake, driven by a command state machine (fill /
// empty the 128-byte sector buffer, read/write a sector by supplying the
// sector then track, read status, read error code). Semantics mirror SimH
// pdp11_rx.c. A single RX01 drive on unit 0; the RX02 double-density option is
// a documented tail.

#define RX_CSR  0177170u // register base (RXCS)
#define RX_VEC  0000264u // interrupt vector
#define RX_IPL  5        // bus request level (BR5)

#define RX_RXCS 0177170u // command/status
#define RX_RXDB 0177172u // data buffer

#define RX_NUMTR 77u  // tracks per disk
#define RX_NUMSC 26u  // sectors per track
#define RX_NUMBY 128u // bytes per sector
#define RX_SIZE  (RX_NUMTR * RX_NUMSC * RX_NUMBY) // bytes per disk (256256)

// RX11 controller state (embedded in the CPU). Single drive 0.
typedef struct {
    uint16_t rxcs;      // control/status
    uint8_t rxdb;       // data buffer
    uint8_t rxes;       // error/status (RXES)
    uint8_t ecode;      // error code
    uint8_t track;      // command track
    uint8_t sector;     // command sector
    uint8_t state;      // command state machine (IDLE/FILL/EMPTY/RWDS/...)
    uint8_t bptr;       // sector-buffer pointer
    uint8_t cur_track;  // drive-0 head position
    uint8_t buf[RX_NUMBY]; // the 128-byte sector buffer
    uint8_t *disk;      // byte backing store (RX_SIZE), or NULL if detached
    uint32_t disk_bytes;
} pdp11_rx11;

// Register access, dispatched from the CPU's I/O-page decode.
uint16_t pdp11_rx_read(pdp11_cpu *cpu, uint16_t addr);
void pdp11_rx_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value);

// Attach drive 0's backing store (a byte buffer of `bytes`, RX_SIZE). NULL
// detaches. The buffer is caller-owned.
void pdp11_rx_attach(pdp11_cpu *cpu, uint8_t *disk, uint32_t bytes);

#endif // PDP11_RX11_H
