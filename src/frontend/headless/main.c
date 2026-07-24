// Deterministic, windowless frontend (emulator-setup-guide.md §7).
//
// It loads a plain-text "image" describing an initial memory/PC state, runs a
// bounded number of instructions, and writes a canonical state dump to stdout.
// The same image drives the SimH oracle harness, and the dump format is what
// tools/regress.py diffs against goldens. No wall clock, no host input.
//
// Image directives (octal values, PDP-11 convention):
//   # comment / blank line   ignored
//   w  ADDR VAL              deposit a word
//   pc VAL                   set the program counter
//   run N                    run up to N instructions (stops early on HALT)
//   dump ADDR N              include N words starting at ADDR in the state dump
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu.h"

#define MAX_DUMP_REGIONS 32

typedef struct {
    uint32_t addr;
    uint32_t count;
} dump_region;

static uint32_t parse_octal(const char *s) {
    return (uint32_t)strtoul(s, NULL, 8);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-file>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open image '%s'\n", argv[1]);
        return 2;
    }

    pdp11_cpu *cpu = pdp11_cpu_create();
    if (cpu == NULL) {
        fprintf(stderr, "out of memory\n");
        fclose(f);
        return 2;
    }

    dump_region regions[MAX_DUMP_REGIONS];
    size_t region_count = 0;
    uint64_t run_limit = 0;

    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char op[16], a[32], b[32];
        int n = sscanf(line, "%15s %31s %31s", op, a, b);
        if (n < 1 || op[0] == '#') {
            continue;
        }
        if (strcmp(op, "w") == 0 && n == 3) {
            pdp11_mem_write_word(cpu->mem, parse_octal(a),
                                 (uint16_t)parse_octal(b));
        } else if (strcmp(op, "pc") == 0 && n >= 2) {
            cpu->r[PDP11_PC] = (uint16_t)parse_octal(a);
        } else if (strcmp(op, "run") == 0 && n >= 2) {
            run_limit = strtoull(a, NULL, 10);
        } else if (strcmp(op, "dump") == 0 && n == 3) {
            if (region_count < MAX_DUMP_REGIONS) {
                regions[region_count].addr = parse_octal(a);
                regions[region_count].count = parse_octal(b);
                region_count++;
            }
        } else {
            fprintf(stderr, "warning: ignoring image line: %s", line);
        }
    }
    fclose(f);

    for (uint64_t i = 0; i < run_limit && !cpu->halted; ++i) {
        pdp11_cpu_step(cpu);
    }

    // stdout carries only oracle-comparable architectural state (registers,
    // PSW, dumped memory) so it diffs byte-for-byte against the SimH oracle.
    // Core-specific run diagnostics go to stderr.
    for (int i = 0; i < 8; ++i) {
        printf("R%d %06o\n", i, cpu->r[i]);
    }
    printf("PSW %06o\n", cpu->psw);
    fprintf(stderr, "HALT %d\n", cpu->halted ? 1 : 0);
    fprintf(stderr, "INSTR %llu\n", (unsigned long long)cpu->instr_count);

    for (size_t r = 0; r < region_count; ++r) {
        for (uint32_t i = 0; i < regions[r].count; ++i) {
            uint32_t addr = regions[r].addr + i * 2u;
            printf("M %06o %06o\n", addr & 0177777u,
                   pdp11_mem_read_word(cpu->mem, addr));
        }
    }

    pdp11_cpu_destroy(cpu);
    return 0;
}
