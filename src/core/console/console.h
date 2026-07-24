#ifndef PDP11_CONSOLE_H
#define PDP11_CONSOLE_H

#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// DL11 console serial line unit (P6). Two independent halves, each a CSR/buffer
// pair on the I/O page, each its own BR4 interrupt:
//   receiver    RCSR 0177560 / RBUF 0177562, vector 060
//   transmitter XCSR 0177564 / XBUF 0177566, vector 064
// CSR bit 7 (DONE) is the ready/done flag, bit 6 (IE) the interrupt enable; only
// IE is writable. Reading RBUF clears the receiver DONE; writing XBUF starts a
// transmit (clears DONE, sets it again a character-time later). Semantics mirror
// SimH pdp11_stddev.c tti_*/tto_*.

#define DL11_RCSR 0177560u
#define DL11_RBUF 0177562u
#define DL11_XCSR 0177564u
#define DL11_XBUF 0177566u
#define DL11_RVEC 0000060u // receiver interrupt vector
#define DL11_XVEC 0000064u // transmitter interrupt vector
#define DL11_IPL  4        // both halves interrupt at BR4

#define DL11_IE   0000100u // CSR<6> interrupt enable
#define DL11_DONE 0000200u // CSR<7> done / ready

// Register access, dispatched from the CPU's I/O-page decode.
uint16_t pdp11_console_read(pdp11_cpu *cpu, uint16_t addr);
void pdp11_console_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value);

// Deliver one received character to the receiver (as if typed at the console):
// latches it in RBUF, sets DONE, and interrupts if enabled.
void pdp11_console_input(pdp11_cpu *cpu, uint8_t ch);

// Register the sink that transmitted characters are written to (the frontend's
// terminal / capture). `ctx` is passed back unchanged.
void pdp11_console_set_sink(pdp11_cpu *cpu, void (*out)(void *ctx, uint8_t ch),
                            void *ctx);

// Advance a pending transmit: when a character-time of emulated time has passed
// since XBUF was written, set the transmitter DONE and interrupt if enabled.
void pdp11_console_tx_poll(pdp11_cpu *cpu);

#endif // PDP11_CONSOLE_H
