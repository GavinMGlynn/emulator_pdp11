#include "clk/clk.h"

#include "cpu/cpu.h"

// The 11/70 has the monitor bit (SimH HAS_LTCM): reads expose DONE and IE, and
// writing a 0 to DONE clears it. Mirrors SimH pdp11_stddev.c clk_rd/clk_wr/svc.

uint16_t pdp11_clk_read(pdp11_cpu *cpu) {
    return (uint16_t)(cpu->clk_csr & (KW11L_DONE | KW11L_IE));
}

void pdp11_clk_write(pdp11_cpu *cpu, uint16_t value) {
    // Only IE is directly writable; DONE is set by the tick and cleared by
    // writing a 0 to it (the monitor bit).
    cpu->clk_csr = (uint16_t)((cpu->clk_csr & ~KW11L_IE) | (value & KW11L_IE));
    if ((value & KW11L_DONE) == 0) {
        cpu->clk_csr &= (uint16_t)~KW11L_DONE;
    }
    // The interrupt line is asserted only while DONE and IE are both set.
    if ((cpu->clk_csr & (KW11L_IE | KW11L_DONE)) == (KW11L_IE | KW11L_DONE)) {
        pdp11_set_int(cpu, PDP11_INT_CLK);
    } else {
        pdp11_clr_int(cpu, PDP11_INT_CLK);
    }
}

void pdp11_clk_tick(pdp11_cpu *cpu) {
    cpu->clk_csr |= KW11L_DONE; // set the monitor/done bit
    if (cpu->clk_csr & KW11L_IE) {
        pdp11_set_int(cpu, PDP11_INT_CLK);
    }
}
