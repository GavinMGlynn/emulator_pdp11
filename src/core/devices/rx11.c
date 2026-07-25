#include "devices/rx11.h"

#include <stddef.h>

#include "cpu/cpu.h"

// Port of SimH pdp11_rx.c. The RX11 is a programmed-I/O device: a command state
// machine moves data a byte at a time through RXDB under the transfer-request
// (TR) handshake. We run the state machine synchronously on register access
// (the per-step floppy delay is not architecturally visible); DONE and the
// interrupt are asserted when a command reaches its final step, exactly as SimH
// does after its scheduled service.

// RXCS bits
#define RXCS_GO    0000001u // go (bit 0)
#define RXCS_FUNC  0000016u // function, bits 3:1
#define RXCS_V_FNC 1
#define RXCS_DRV   0000020u // drive select (bit 4)
#define RXCS_DONE  0000040u // done (bit 5)
#define RXCS_IE    0000100u // interrupt enable (bit 6)
#define RXCS_TR    0000200u // transfer request (bit 7)
#define RXCS_INIT  0040000u // initialize (bit 14)
#define RXCS_ERR   0100000u // error (bit 15)
#define RXCS_ROUT  (RXCS_ERR | RXCS_TR | RXCS_IE | RXCS_DONE) // readable bits
#define RXCS_RW    RXCS_IE   // program-writable (outside a GO)

// Function codes (RXCS<3:1>)
#define FUNC_FILL  0
#define FUNC_EMPTY 1
#define FUNC_WRITE 2
#define FUNC_READ  3
#define FUNC_RDST  5 // read status
#define FUNC_WRDEL 6 // write deleted-data sector
#define FUNC_ECODE 7 // read error code

// RXES (error/status) bits
#define RXES_ID   0004u // initialize done
#define RXES_WLK  0010u // write locked
#define RXES_DD   0100u // deleted data
#define RXES_DRDY 0200u // drive ready

// Command state machine
#define ST_IDLE    0
#define ST_RWDS    1 // read/write: awaiting the sector byte
#define ST_RWDT    2 // read/write: awaiting the track byte
#define ST_RWXFR   3 // read/write: perform the sector transfer
#define ST_FILL    4 // fill the sector buffer from the program
#define ST_EMPTY   5 // empty the sector buffer to the program
#define ST_CMDDONE 6 // status/error command completes

static int rx_func(const pdp11_rx11 *rx) {
    return (rx->rxcs >> RXCS_V_FNC) & 07;
}

// Complete a command: go idle, set DONE (+ interrupt if IE), fold flags into
// RXES, and post the error code / RXDB. new_ecode < 0 means don't touch RXDB.
static void rx_done(pdp11_cpu *cpu, uint8_t esr_flags, int new_ecode) {
    pdp11_rx11 *rx = &cpu->rx;
    rx->state = ST_IDLE;
    rx->rxcs |= RXCS_DONE;
    if (rx->rxcs & RXCS_IE) {
        pdp11_set_int(cpu, PDP11_INT_RX);
    }
    rx->rxes = (uint8_t)((rx->rxes | esr_flags) & ~RXES_DRDY);
    if (rx->disk != NULL) {
        rx->rxes |= RXES_DRDY;
    }
    if (new_ecode > 0) {
        rx->rxcs |= RXCS_ERR;
    }
    if (new_ecode < 0) {
        return;
    }
    rx->ecode = (uint8_t)new_ecode;
    rx->rxdb = rx->rxes; // RXDB reflects RXES at command completion
}

// Advance the state machine one step (SimH rx_svc).
static void rx_step(pdp11_cpu *cpu) {
    pdp11_rx11 *rx = &cpu->rx;
    int func = rx_func(rx);
    switch (rx->state) {
    case ST_EMPTY:
        if (rx->bptr >= RX_NUMBY) {
            rx_done(cpu, 0, 0);
        } else {
            rx->rxdb = rx->buf[rx->bptr++];
            rx->rxcs |= RXCS_TR;
        }
        break;
    case ST_FILL:
        rx->buf[rx->bptr++] = rx->rxdb;
        if (rx->bptr < RX_NUMBY) {
            rx->rxcs |= RXCS_TR;
        } else {
            rx_done(cpu, 0, 0);
        }
        break;
    case ST_RWDS:
        rx->sector = (uint8_t)(rx->rxdb & 0177u);
        rx->rxcs |= RXCS_TR;
        rx->state = ST_RWDT;
        break;
    case ST_RWDT:
        rx->track = rx->rxdb;
        rx->state = ST_RWXFR;
        rx_step(cpu); // perform the transfer now
        break;
    case ST_RWXFR: {
        if (rx->disk == NULL) { rx_done(cpu, 0, 0110); break; }   // drive not ready
        if (rx->track >= RX_NUMTR) { rx_done(cpu, 0, 0040); break; } // bad track
        rx->cur_track = rx->track;
        if (rx->sector == 0 || rx->sector > RX_NUMSC) { rx_done(cpu, 0, 0070); break; }
        uint32_t da = ((uint32_t)rx->track * RX_NUMSC + (rx->sector - 1u)) * RX_NUMBY;
        if (func == FUNC_WRDEL) {
            rx->rxes |= RXES_DD;
        }
        if (func == FUNC_READ) {
            for (uint32_t i = 0; i < RX_NUMBY; ++i) {
                rx->buf[i] = rx->disk[da + i];
            }
        } else { // WRITE / WRDEL
            for (uint32_t i = 0; i < RX_NUMBY; ++i) {
                rx->disk[da + i] = rx->buf[i];
            }
        }
        rx_done(cpu, 0, 0);
        break;
    }
    case ST_CMDDONE:
        if (func == FUNC_ECODE) {
            rx->rxdb = rx->ecode;
            rx_done(cpu, 0, -1); // don't overwrite RXDB
        } else {
            rx_done(cpu, 0, 0);  // read status: RXDB <- RXES
        }
        break;
    default:
        break;
    }
}

static void rx_reset_state(pdp11_cpu *cpu) {
    pdp11_rx11 *rx = &cpu->rx;
    rx->rxcs = RXCS_DONE;
    rx->rxes = RXES_ID | (rx->disk != NULL ? RXES_DRDY : 0u);
    rx->rxdb = rx->rxes;
    rx->ecode = 0;
    rx->state = ST_IDLE;
    rx->bptr = 0;
    pdp11_clr_int(cpu, PDP11_INT_RX);
}

uint16_t pdp11_rx_read(pdp11_cpu *cpu, uint16_t addr) {
    pdp11_rx11 *rx = &cpu->rx;
    if (addr == RX_RXCS) {
        return (uint16_t)(rx->rxcs & RXCS_ROUT); // only ERR/TR/IE/DONE read back
    }
    // RXDB: return the current byte; if emptying, advance to the next.
    uint8_t d = rx->rxdb;
    if (rx->state == ST_EMPTY && (rx->rxcs & RXCS_TR)) {
        rx->rxcs &= (uint16_t)~RXCS_TR;
        rx_step(cpu);
    }
    return d;
}

void pdp11_rx_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value) {
    pdp11_rx11 *rx = &cpu->rx;
    if (addr == RX_RXCS) {
        if (value & RXCS_INIT) { // controller initialize
            rx_reset_state(cpu);
            return;
        }
        if ((value & RXCS_GO) && rx->state == ST_IDLE) { // launch a function
            rx->rxcs = (uint16_t)(value & (RXCS_IE | RXCS_DRV | RXCS_FUNC));
            rx->bptr = 0;
            switch (rx_func(rx)) {
            case FUNC_FILL:
                rx->state = ST_FILL;
                rx->rxcs |= RXCS_TR;
                break;
            case FUNC_EMPTY:
                rx->state = ST_EMPTY;
                rx_step(cpu); // load the first byte, set TR
                break;
            case FUNC_READ:
            case FUNC_WRITE:
            case FUNC_WRDEL:
                rx->state = ST_RWDS;
                rx->rxcs |= RXCS_TR;
                rx->rxes = (uint8_t)(rx->rxes & RXES_ID); // clear errors
                break;
            default: // read status / read error code / (RX02 set density)
                rx->state = ST_CMDDONE;
                rx_step(cpu);
                break;
            }
            return;
        }
        // Not a GO: just update interrupt-enable.
        if (!(value & RXCS_IE)) {
            pdp11_clr_int(cpu, PDP11_INT_RX);
        } else if ((rx->rxcs & (RXCS_DONE | RXCS_IE)) == RXCS_DONE) {
            pdp11_set_int(cpu, PDP11_INT_RX);
        }
        rx->rxcs = (uint16_t)((rx->rxcs & ~RXCS_RW) | (value & RXCS_RW));
        return;
    }
    // RXDB: accept a byte only when a command has requested a transfer.
    if (rx->state != ST_IDLE && (rx->rxcs & RXCS_TR)) {
        rx->rxdb = (uint8_t)(value & 0377u);
        if (rx->state != ST_EMPTY) {
            rx->rxcs &= (uint16_t)~RXCS_TR;
            rx_step(cpu);
        }
    }
}

void pdp11_rx_attach(pdp11_cpu *cpu, uint8_t *disk, uint32_t bytes) {
    cpu->rx.disk = disk;
    cpu->rx.disk_bytes = bytes;
}
