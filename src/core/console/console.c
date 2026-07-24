#include "console/console.h"

#include "cpu/cpu.h"

// A transmitted character is emitted to the sink immediately (so output survives
// even if the program halts before the transmit "completes"); the DONE/interrupt
// completion then follows a character-time later in emulated time. The exact
// delay is not architecturally visible — SimH's is wall-clock-calibrated — so it
// is a nominal ~9600-baud character time (about 1 ms).
#define DL11_TX_NS 1000000u

void pdp11_console_set_sink(pdp11_cpu *cpu, void (*out)(void *ctx, uint8_t ch),
                            void *ctx) {
    cpu->console_out = out;
    cpu->console_ctx = ctx;
}

void pdp11_console_input(pdp11_cpu *cpu, uint8_t ch) {
    cpu->tti_buf = ch;
    cpu->tti_csr |= DL11_DONE;
    if (cpu->tti_csr & DL11_IE) {
        pdp11_set_int(cpu, PDP11_INT_TTI);
    }
}

void pdp11_console_tx_poll(pdp11_cpu *cpu) {
    if (cpu->tto_busy && cpu->time_ns >= cpu->tto_done_ns) {
        cpu->tto_busy = false;
        cpu->tto_csr |= DL11_DONE;
        if (cpu->tto_csr & DL11_IE) {
            pdp11_set_int(cpu, PDP11_INT_TTO);
        }
    }
}

uint16_t pdp11_console_read(pdp11_cpu *cpu, uint16_t addr) {
    switch (addr) {
    case DL11_RCSR:
        return (uint16_t)(cpu->tti_csr & (DL11_DONE | DL11_IE));
    case DL11_RBUF: // reading the buffer clears receiver DONE and its interrupt
        cpu->tti_csr &= (uint16_t)~DL11_DONE;
        pdp11_clr_int(cpu, PDP11_INT_TTI);
        return (uint16_t)(cpu->tti_buf & 0377u);
    case DL11_XCSR:
        return (uint16_t)(cpu->tto_csr & (DL11_DONE | DL11_IE));
    case DL11_XBUF:
        return (uint16_t)(cpu->tto_buf & 0377u);
    default:
        return 0;
    }
}

void pdp11_console_write(pdp11_cpu *cpu, uint16_t addr, uint16_t value) {
    switch (addr) {
    case DL11_RCSR:
        // Only IE is writable. Enabling it while DONE is already set raises the
        // interrupt; disabling it drops the request.
        if ((value & DL11_IE) == 0) {
            pdp11_clr_int(cpu, PDP11_INT_TTI);
        } else if ((cpu->tti_csr & (DL11_DONE | DL11_IE)) == DL11_DONE) {
            pdp11_set_int(cpu, PDP11_INT_TTI);
        }
        cpu->tti_csr = (uint16_t)((cpu->tti_csr & ~DL11_IE) | (value & DL11_IE));
        break;
    case DL11_RBUF:
        break; // read-only
    case DL11_XCSR:
        if ((value & DL11_IE) == 0) {
            pdp11_clr_int(cpu, PDP11_INT_TTO);
        } else if ((cpu->tto_csr & (DL11_DONE | DL11_IE)) == DL11_DONE) {
            pdp11_set_int(cpu, PDP11_INT_TTO);
        }
        cpu->tto_csr = (uint16_t)((cpu->tto_csr & ~DL11_IE) | (value & DL11_IE));
        break;
    case DL11_XBUF:
        cpu->tto_buf = (uint16_t)(value & 0377u);
        if (cpu->console_out) {
            cpu->console_out(cpu->console_ctx, (uint8_t)(value & 0377u));
        }
        // Transmit starts: clear DONE now, schedule its return one char-time on.
        cpu->tto_csr &= (uint16_t)~DL11_DONE;
        pdp11_clr_int(cpu, PDP11_INT_TTO);
        cpu->tto_busy = true;
        cpu->tto_done_ns = cpu->time_ns + DL11_TX_NS;
        break;
    default:
        break;
    }
}
