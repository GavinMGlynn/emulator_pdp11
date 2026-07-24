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

#include "console/console.h"
#include "cpu/cpu.h"
#include "devices/rk11.h"

#define MAX_DUMP_REGIONS 32

typedef struct {
    uint32_t addr;
    uint32_t count;
} dump_region;

static uint32_t parse_octal(const char *s) {
    return (uint32_t)strtoul(s, NULL, 8);
}

// ---- boot mode (P7) --------------------------------------------------------
// Attach an RK05 .dsk image, deposit the standard RK boot ROM (which reads block
// 0 to memory 0 and jumps there — identical to SimH's rk_boot), capture the DL11
// console to stdout, and inject scripted keyboard input. This drives a real Unix
// boot as the integration thermometer; the console stream diffs against SimH.

// SimH's RK boot ROM (pdp11_rk.c boot_rom), loaded at 02000, entry 02002.
#define BOOT_START 02000u
static const uint16_t rk_boot_rom[] = {
    0042113,                    // "KD"
    0012706, BOOT_START,        // MOV #boot_start, SP
    0012700, 0000000,           // MOV #unit, R0
    0010003,                    // MOV R0, R3
    0000303,                    // SWAB R3
    0006303, 0006303, 0006303, 0006303, 0006303, // ASL R3 x5
    0012701, 0177412,           // MOV #RKDA, R1
    0010311,                    // MOV R3, (R1)   ; load disk address (drive)
    0005041,                    // CLR -(R1)      ; clear bus address
    0012741, 0177000,           // MOV #-256.*2, -(R1) ; word count
    0012741, 0000005,           // MOV #READ+GO, -(R1) ; command
    0005002,                    // CLR R2
    0005003,                    // CLR R3
    0012704, BOOT_START + 020u, // MOV #START+20, R4
    0005005,                    // CLR R5
    0105711,                    // TSTB (R1)      ; wait for done
    0100376,                    // BPL .-2
    0105011,                    // CLRB (R1)
    0005007,                    // CLR PC         ; jump to the loaded block 0
};

static void console_sink(void *ctx, uint8_t ch) {
    (void)ctx;
    // The DL11 transmits 8 bits; V6's console driver puts even parity in bit 7.
    // A KSR-33/VT-style console is a 7-bit ASCII device, so strip the parity bit
    // for display — matching SimH's default 7-bit terminal (so the boot stream
    // diffs cleanly against the oracle).
    putchar(ch & 0177);
    fflush(stdout);
}

// Convert C-style escapes (\r \n \t \\) in an input script to raw bytes.
static size_t unescape(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; s[i] && n < cap; ++i) {
        if (s[i] == '\\' && s[i + 1]) {
            char c = s[++i];
            out[n++] = (uint8_t)(c == 'r' ? '\r' : c == 'n' ? '\n'
                                 : c == 't' ? '\t' : c);
        } else {
            out[n++] = (uint8_t)s[i];
        }
    }
    return n;
}

static int run_boot(const char *disk_path, uint64_t max_instr,
                    const char *in_script) {
    FILE *df = fopen(disk_path, "rb");
    if (df == NULL) {
        fprintf(stderr, "cannot open disk image '%s'\n", disk_path);
        return 2;
    }
    fseek(df, 0, SEEK_END);
    long bytes = ftell(df);
    fseek(df, 0, SEEK_SET);
    uint32_t words = (uint32_t)(bytes / 2);
    uint16_t *disk = calloc(RK_WORDS > words ? RK_WORDS : words, sizeof(uint16_t));
    if (disk == NULL) {
        fclose(df);
        fprintf(stderr, "out of memory for disk\n");
        return 2;
    }
    for (uint32_t i = 0; i < words; ++i) {
        int lo = fgetc(df), hi = fgetc(df);
        if (hi == EOF) {
            break;
        }
        disk[i] = (uint16_t)(lo | (hi << 8));
    }
    fclose(df);

    pdp11_cpu *cpu = pdp11_cpu_create();
    if (cpu == NULL) {
        free(disk);
        return 2;
    }
    pdp11_console_set_sink(cpu, console_sink, NULL);
    pdp11_rk_attach(cpu, disk, RK_WORDS > words ? RK_WORDS : words);

    // Deposit the boot ROM and start at its entry (02002).
    for (size_t i = 0; i < sizeof rk_boot_rom / sizeof rk_boot_rom[0]; ++i) {
        pdp11_mem_write_word(cpu->mem, BOOT_START + (uint32_t)(i * 2),
                             rk_boot_rom[i]);
    }
    cpu->r[PDP11_PC] = (uint16_t)(BOOT_START + 2u);

    uint8_t input[512];
    size_t in_len = in_script ? unescape(in_script, input, sizeof input) : 0;
    size_t in_pos = 0;

    for (uint64_t i = 0; i < max_instr && !cpu->halted; ++i) {
        pdp11_cpu_step(cpu);
        // Feed the next scripted key once the receiver buffer is empty.
        if (in_pos < in_len && !(cpu->tti_csr & DL11_DONE)) {
            pdp11_console_input(cpu, input[in_pos++]);
        }
    }
    fprintf(stderr, "\n[boot: halted=%d instr=%llu pc=%06o]\n",
            cpu->halted ? 1 : 0, (unsigned long long)cpu->instr_count,
            cpu->r[PDP11_PC]);
    pdp11_cpu_destroy(cpu);
    free(disk);
    return 0;
}

int main(int argc, char **argv) {
    // Boot mode: pdp11_headless --boot-rk <disk> [--max N] [--in <script>]
    if (argc >= 3 && strcmp(argv[1], "--boot-rk") == 0) {
        const char *disk = argv[2];
        uint64_t max_instr = 50000000ull;
        const char *in_script = NULL;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--max") == 0) {
                max_instr = strtoull(argv[i + 1], NULL, 10);
            } else if (strcmp(argv[i], "--in") == 0) {
                in_script = argv[i + 1];
            }
        }
        return run_boot(disk, max_instr, in_script);
    }

    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-file>\n", argv[0]);
        fprintf(stderr, "   or: %s --boot-rk <disk.dsk> [--max N] [--in script]\n",
                argv[0]);
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
