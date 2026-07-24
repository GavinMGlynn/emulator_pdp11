#ifndef PDP11_CLK_H
#define PDP11_CLK_H

#include <stdint.h>

typedef struct pdp11_cpu pdp11_cpu;

// KW11-L line-frequency clock (P6). A single status register (LKS) at 0177546;
// each line-frequency tick sets the monitor/done bit and, when interrupts are
// enabled, requests a BR6 interrupt through vector 0100. On the 11/70 (which
// has the monitor bit, HAS_LTCM) the done bit is readable and is cleared by
// writing a 0 to it; only the interrupt-enable bit is otherwise writable.
#define KW11L_LKS  0177546u // line-clock status register (LKS)
#define KW11L_VEC  0000100u // interrupt vector
#define KW11L_IPL  6        // bus request level (BR6)

#define KW11L_IE   0000100u // CSR<6> interrupt enable
#define KW11L_DONE 0000200u // CSR<7> monitor / done (set by each tick)

// Register access (dispatched from the CPU's I/O-page decode).
uint16_t pdp11_clk_read(pdp11_cpu *cpu);
void pdp11_clk_write(pdp11_cpu *cpu, uint16_t value);

// Model one line-frequency tick: set the done bit and, if enabled, interrupt.
void pdp11_clk_tick(pdp11_cpu *cpu);

#endif // PDP11_CLK_H
