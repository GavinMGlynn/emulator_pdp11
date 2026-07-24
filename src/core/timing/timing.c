#include "timing/timing.h"

#include <stdbool.h>

// Source / Destination Address Time by addressing mode (C.1.5 / C.1.6), in ns,
// with the number of read memory cycles each contributes. Both tables are
// identical on the 11/70.
static const uint32_t addr_ns[8] = {0, 300, 300, 750, 450, 900, 600, 1050};
static const uint32_t addr_cyc[8] = {0, 1, 1, 2, 1, 2, 2, 3};

// Instruction classes for timing (EF-time selection).
enum {
    CLS_MOV, CLS_ADD, CLS_CMP, CLS_XOR, // double-operand
    CLS_SINGLE_STD,   // CLR/COM/INC/DEC/ADC/SBC/ROL/ASL/SWAB/SXT
    CLS_SINGLE_NEG, CLS_SINGLE_TST, CLS_SINGLE_SHIFT, // NEG / TST / ROR,ASR
    CLS_OTHER
};

// EF time (Execute/Fetch) for a double-operand instruction (C.1.7), given its
// class and source/destination fields. Adds the documented note amounts.
static pdp11_timing ef_double(int cls, int smode, int sreg, int dmode, int dreg,
                              bool bytemode) {
    pdp11_timing t = {0, 0};
    if (cls == CLS_MOV) {
        // MOV has its own EF table indexed by DST mode/register; no DST Time.
        static const uint32_t mov_ns[8][2] = {
            {300, 450}, {1200, 1200}, {1200, 1200}, {1650, 1650},
            {1350, 1350}, {1800, 1800}, {1500, 1650}, {1950, 2100}};
        static const uint32_t mov_cyc[8] = {1, 1, 1, 2, 1, 2, 2, 3};
        int col = (smode == 0) ? 0 : 1;
        if (dmode == 0 && dreg == 7) { // MOV to PC row
            t.ns = (smode == 0) ? 600 : 750;
            t.read_cycles = 1;
        } else {
            t.ns = mov_ns[dmode][col];
            t.read_cycles = mov_cyc[dmode];
        }
        return t;
    }

    if (dmode == 0) { // DST is a register
        t.ns = (smode == 0) ? 300 : 450;
        t.read_cycles = (smode == 0) ? 1u : 2u;
        if (cls == CLS_XOR) {
            t.ns = 300; // XOR is 0.30 in both DST-mode-0 columns
        }
        if (dreg == 7) {
            t.ns += 300; // NOTE (D): +0.30 us if DST is R7
        }
    } else { // DST mode 1-7
        t.read_cycles = 1;
        switch (cls) {
        case CLS_ADD: t.ns = 1200; break;
        case CLS_CMP: t.ns = 450;  break;
        case CLS_XOR: t.ns = 1200; break;
        default:      t.ns = 1200; break;
        }
        // NOTE (C): +0.15 us if SRC is a register R1-R7 and DST base is R6/R7.
        if (smode == 0 && sreg >= 1 && (dreg == 6 || dreg == 7)) {
            t.ns += 150;
        }
    }
    (void)bytemode;
    return t;
}

// EF time for a single-operand instruction (C.1.7), given class and DST mode.
static pdp11_timing ef_single(int cls, int dmode, int dreg, bool bytemode) {
    pdp11_timing t = {0, 1};
    bool reg = (dmode == 0);
    switch (cls) {
    case CLS_SINGLE_STD:
        t.ns = reg ? 300u : 1200u;
        if (reg && dreg == 7) {
            t.ns += 300; // NOTE (J): +0.30 us if DST is R7
        }
        break;
    case CLS_SINGLE_NEG:   t.ns = reg ? 750u : 1500u; break;
    case CLS_SINGLE_TST:   t.ns = reg ? 300u : 450u;  break;
    case CLS_SINGLE_SHIFT: // ROR/ASR
        t.ns = reg ? 300u : 1200u;
        if (reg && dreg == 7) {
            t.ns += 300; // NOTE (J)
        }
        if (!reg && bytemode) {
            t.ns += 150; // NOTE (H): odd byte
        }
        break;
    default: t.ns = 300; break;
    }
    return t;
}

// Classify a double-operand top nibble (1-6 word, 9-14 byte) to a timing class.
static int classify_double(uint8_t top) {
    switch (top) {
    case 001: case 011: return CLS_MOV; // MOV / MOVB (MOVB regroups below)
    case 002: case 012: return CLS_CMP; // CMP / CMPB
    case 003: case 013: return CLS_CMP; // BIT / BITB
    case 004: case 014: return CLS_ADD; // BIC / BICB
    case 005: case 015: return CLS_ADD; // BIS / BISB
    case 006:           return CLS_ADD; // ADD
    case 016:           return CLS_ADD; // SUB
    default:            return CLS_OTHER;
    }
}

pdp11_timing pdp11_instr_timing(uint16_t word) {
    uint8_t top = (uint8_t)((word >> 12) & 017u);
    int smode = (word >> 9) & 07;
    int sreg = (word >> 6) & 07;
    int dmode = (word >> 3) & 07;
    int dreg = word & 07;
    bool bytemode = false;

    // Double-operand instructions.
    if ((top >= 001 && top <= 006) || (top >= 011 && top <= 016)) {
        bytemode = (top >= 011 && top <= 015);
        int cls = classify_double(top);
        // MOVB is timed with the ADD class (SRC+DST+EF), unlike MOV.
        if (top == 011) {
            cls = CLS_ADD;
        }
        pdp11_timing t = {0, 0};
        // SRC Time applies to every double-operand instruction.
        t.ns += addr_ns[smode];
        t.read_cycles += addr_cyc[smode];
        // DST Time applies to all except MOV (word).
        if (top != 001) {
            t.ns += addr_ns[dmode];
            t.read_cycles += addr_cyc[dmode];
            if (bytemode && dmode != 0) {
                t.ns += 150; // NOTE (A): odd byte, except DST mode 0
            }
        }
        pdp11_timing ef = ef_double(cls, smode, sreg, dmode, dreg, bytemode);
        t.ns += ef.ns;
        t.read_cycles += ef.read_cycles;
        return t;
    }

    // JMP (0001DD) and JSR (004RDD): Instruction Time by DST mode (C-5).
    if ((word & 0177700u) == 0000100u) { // JMP
        static const uint32_t jmp_ns[8] = {0, 900, 900, 1200, 900, 1350, 1050,
                                            1500};
        static const uint32_t jmp_cyc[8] = {0, 1, 1, 2, 1, 2, 2, 3};
        pdp11_timing t = {jmp_ns[dmode], jmp_cyc[dmode]};
        return t;
    }
    if ((word & 0177000u) == 0004000u) { // JSR
        static const uint32_t jsr_ns[8] = {0, 1950, 1950, 2250, 1950, 2400,
                                            2100, 2550};
        static const uint32_t jsr_cyc[8] = {0, 1, 1, 2, 1, 2, 2, 3};
        pdp11_timing t = {jsr_ns[dmode], jsr_cyc[dmode]};
        return t;
    }

    // EIS / XOR (top nibble 7). MUL and XOR are exact; DIV and ASH/ASHC are
    // operand/shift-count dependent (P4b tail — the handbook gives only a range
    // for DIV), so a representative time is used and noted.
    if (top == 007) {
        int op = (word >> 9) & 07; // IR<11:9>
        if (op == 0) { // MUL: SRC time + 3.30 us
            pdp11_timing t = {addr_ns[dmode] + 3300, addr_cyc[dmode] + 1};
            return t;
        }
        if (op == 4) { // XOR: R (reg) + DST time + EF
            pdp11_timing t = {addr_ns[dmode] + (dmode == 0 ? 300u : 1200u),
                              addr_cyc[dmode] + 1};
            return t;
        }
        // DIV (1), ASH (2), ASHC (3): representative, refined at P4b tail.
        pdp11_timing t = {addr_ns[dmode] + 3300, addr_cyc[dmode] + 1};
        return t;
    }

    // Single-operand instructions (word 0050-0067, byte 1050-1067).
    uint16_t code = (uint16_t)((word >> 6) & 01777u);
    uint16_t base = (uint16_t)(code & 0777u);
    bytemode = (code & 01000u) != 0;

    // MFPI/MFPD (0065): "use with SRC times" + 1.50 EF (C-4).
    if (base == 0065) {
        pdp11_timing t = {addr_ns[dmode] + 1500, addr_cyc[dmode] + 1};
        return t;
    }
    // MTPI/MTPD (0066): Instruction Time by DST mode (C-5).
    if (base == 0066) {
        static const uint32_t mtp_ns[8] = {900, 1650, 1650, 2100, 1800, 2250,
                                           2100, 2550};
        static const uint32_t mtp_cyc[8] = {1, 2, 2, 3, 2, 3, 3, 4};
        pdp11_timing t = {mtp_ns[dmode], mtp_cyc[dmode]};
        return t;
    }

    if (base >= 0050 && base <= 0067) {
        int cls;
        switch (base) {
        case 0054: cls = CLS_SINGLE_NEG; break;
        case 0057: cls = CLS_SINGLE_TST; break;
        case 0060: case 0062: cls = CLS_SINGLE_SHIFT; break; // ROR/ASR
        default:   cls = CLS_SINGLE_STD; break;
        }
        pdp11_timing t = {addr_ns[dmode], addr_cyc[dmode]}; // DST Time
        if (bytemode && dmode != 0) {
            t.ns += 150; // NOTE (A)
        }
        pdp11_timing ef = ef_single(cls, dmode, dreg, bytemode);
        t.ns += ef.ns;
        t.read_cycles += ef.read_cycles;
        return t;
    }

    // Branches, jumps, EIS, FP, traps: their EF tables land in P4b. Until then a
    // nominal single-fetch time keeps the accumulator monotonic.
    pdp11_timing t = {300, 1};
    return t;
}
