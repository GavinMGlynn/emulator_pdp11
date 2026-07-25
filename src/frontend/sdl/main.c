// Interactive SDL3 frontend (P8): a VT-style terminal driven by the DL11
// console, plus a KY11-style console panel showing the address/data lights and a
// RUN indicator. The core is unchanged and unaware of us — we only feed it typed
// characters and consume transmitted ones, exactly like the headless frontend.
//
//   pdp11_sdl [--boot-rk <disk.dsk>] [--frames N] [--scale S] [--ips N]
//
// With --boot-rk it deposits SimH's RK bootstrap and boots the attached image
// (type `unix` etc. at the console). Without a disk it runs a tiny built-in
// banner program so the window shows something. --frames N renders exactly N
// frames and exits (0) — used under SDL_VIDEODRIVER=dummy for a headless CI
// smoke test.
#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console/console.h"
#include "cpu/cpu.h"
#include "devices/rk11.h"

// ---- text terminal model --------------------------------------------------
#define TCOLS 80
#define TROWS 25

typedef struct {
    char cell[TROWS][TCOLS];
    int cx, cy;
} vt_term;

static void vt_init(vt_term *t) {
    memset(t->cell, ' ', sizeof t->cell);
    t->cx = 0;
    t->cy = 0;
}

static void vt_scroll(vt_term *t) {
    memmove(&t->cell[0][0], &t->cell[1][0], (size_t)(TROWS - 1) * TCOLS);
    memset(&t->cell[TROWS - 1][0], ' ', TCOLS);
}

static void vt_newline(vt_term *t) {
    t->cy++;
    if (t->cy >= TROWS) {
        t->cy = TROWS - 1;
        vt_scroll(t);
    }
}

static void vt_putc(vt_term *t, char c) {
    switch (c) {
    case '\r': t->cx = 0; return;
    case '\n': vt_newline(t); return;
    case '\b': if (t->cx > 0) { t->cx--; } return;
    case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx >= TCOLS) { t->cx = TCOLS - 1; } return;
    case 0x07: return; // bell
    default: break;
    }
    if (c < 32 || c > 126) {
        return;
    }
    t->cell[t->cy][t->cx] = c;
    if (++t->cx >= TCOLS) {
        t->cx = 0;
        vt_newline(t);
    }
}

static void console_sink(void *ctx, uint8_t ch) {
    // The DL11 carries 8 bits (V6 puts even parity in bit 7); a 7-bit terminal
    // shows the low 7, matching the headless frontend and SimH.
    vt_putc((vt_term *)ctx, (char)(ch & 0177u));
}

// ---- boot support ---------------------------------------------------------
// SimH's RK bootstrap, identical to the headless frontend: reads block 0 to
// address 0 and jumps there. Entry at 02002.
#define BOOT_START 02000u
static const uint16_t rk_boot_rom[] = {
    0042113, 0012706, BOOT_START, 0012700, 0000000, 0010003, 0000303,
    0006303, 0006303, 0006303, 0006303, 0006303, 0012701, 0177412,
    0010311, 0005041, 0012741, 0177000, 0012741, 0000005, 0005002,
    0005003, 0012704, BOOT_START + 020u, 0005005, 0105711, 0100376,
    0105011, 0005007,
};

// A tiny stand-in program (no disk): poll the DL11 transmitter and print a
// banner, then HALT. Lets the window show text without a copyrighted image.
#define DEMO_START 01000u
static const uint16_t demo_rom[] = {
    0012701, 0001040,        // 01000: MOV #msg, R1
    0112100,                 // 01004: L: MOVB (R1)+, R0
    0001406,                 // 01006: BEQ done  (-> 01024)
    0105737, 0177564,        // 01010: W: TSTB @#177564  (XCSR)
    0100375,                 // 01014: BPL W
    0110037, 0177566,        // 01016: MOVB R0, @#177566 (XBUF)
    0000770,                 // 01022: BR L
    0000000,                 // 01024: done: HALT
};
// The banner string, deposited as bytes starting at 01040.
static const char demo_msg[] = "PDP-11/70 -- SDL console\r\n";

static void deposit(pdp11_cpu *cpu, uint32_t addr, const uint16_t *w, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        pdp11_mem_write_word(cpu->mem, addr + (uint32_t)(i * 2), w[i]);
    }
}

// Load a .dsk into a freshly allocated RK image; returns the buffer (caller
// frees) or NULL on failure. `*out_words` gets the image size in words.
static uint16_t *load_disk(const char *path, uint32_t *out_words) {
    FILE *df = fopen(path, "rb");
    if (df == NULL) {
        fprintf(stderr, "cannot open disk image '%s'\n", path);
        return NULL;
    }
    fseek(df, 0, SEEK_END);
    long bytes = ftell(df);
    fseek(df, 0, SEEK_SET);
    uint32_t words = (uint32_t)(bytes / 2);
    uint32_t cap = RK_WORDS > words ? RK_WORDS : words;
    uint16_t *disk = calloc(cap, sizeof *disk);
    if (disk != NULL) {
        for (uint32_t i = 0; i < words; ++i) {
            int lo = fgetc(df), hi = fgetc(df);
            if (hi == EOF) {
                break;
            }
            disk[i] = (uint16_t)(lo | (hi << 8));
        }
    }
    fclose(df);
    *out_words = cap;
    return disk;
}

// ---- rendering ------------------------------------------------------------
#define GLYPH 8 // SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE
#define MARGIN 8
#define WIN_W (MARGIN * 2 + TCOLS * GLYPH)
#define PANEL_H 40
#define WIN_H (MARGIN * 3 + TROWS * GLYPH + PANEL_H)

static void draw_text(SDL_Renderer *r, float x, float y, const char *s) {
    SDL_RenderDebugText(r, x, y, s);
}

// Render 16 lamps for a value, MSB left, lit lamps brighter.
static void draw_lamps(SDL_Renderer *r, float x, float y, uint16_t v) {
    for (int b = 15; b >= 0; --b) {
        bool on = (v >> b) & 1u;
        if (on) {
            SDL_SetRenderDrawColor(r, 255, 140, 0, 255);
        } else {
            SDL_SetRenderDrawColor(r, 60, 30, 0, 255);
        }
        SDL_FRect lamp = {x + (float)(15 - b) * 9.0f, y, 6.0f, 8.0f};
        SDL_RenderFillRect(r, &lamp);
    }
}

static void render(SDL_Renderer *r, const vt_term *t, const pdp11_cpu *cpu,
                   uint64_t instrs) {
    SDL_SetRenderDrawColor(r, 12, 12, 16, 255);
    SDL_RenderClear(r);

    // Terminal: amber-green on near-black.
    SDL_SetRenderDrawColor(r, 120, 255, 140, 255);
    char line[TCOLS + 1];
    for (int row = 0; row < TROWS; ++row) {
        memcpy(line, t->cell[row], TCOLS);
        line[TCOLS] = '\0';
        draw_text(r, (float)MARGIN, (float)(MARGIN + row * GLYPH), line);
    }

    // Console panel below a divider line.
    float py = (float)(MARGIN * 2 + TROWS * GLYPH);
    SDL_SetRenderDrawColor(r, 40, 40, 48, 255);
    SDL_FRect div = {0.0f, py - (float)MARGIN / 2.0f, (float)WIN_W, 1.0f};
    SDL_RenderFillRect(r, &div);

    SDL_SetRenderDrawColor(r, 180, 180, 190, 255);
    char panel[64];
    snprintf(panel, sizeof panel, "ADDR");
    draw_text(r, (float)MARGIN, py, panel);
    draw_lamps(r, (float)(MARGIN + 6 * GLYPH), py, cpu->r[PDP11_PC]);
    snprintf(panel, sizeof panel, "DATA");
    draw_text(r, (float)MARGIN, py + (float)GLYPH * 1.5f, panel);
    draw_lamps(r, (float)(MARGIN + 6 * GLYPH), py + (float)GLYPH * 1.5f, cpu->psw);

    SDL_SetRenderDrawColor(r, cpu->halted ? 90 : 255, cpu->halted ? 90 : 60,
                           60, 255);
    SDL_FRect run = {(float)(WIN_W - MARGIN - 10), py, 10.0f, 10.0f};
    SDL_RenderFillRect(r, &run);
    SDL_SetRenderDrawColor(r, 180, 180, 190, 255);
    snprintf(panel, sizeof panel, "PC %06o PSW %06o %s  %lluK",
             cpu->r[PDP11_PC], cpu->psw, cpu->halted ? "HALT" : "RUN ",
             (unsigned long long)(instrs / 1000u));
    draw_text(r, (float)(WIN_W - MARGIN - 34 * GLYPH), py + (float)GLYPH * 1.5f,
              panel);

    SDL_RenderPresent(r);
}

// Map an SDL keycode to the byte the DL11 should receive, or -1 for none.
static int key_to_byte(SDL_Keycode key) {
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return '\r';
    case SDLK_BACKSPACE: return 0177; // DEL, the V6 erase char
    case SDLK_TAB:       return '\t';
    case SDLK_ESCAPE:    return 033;
    default:             return -1;
    }
}

int main(int argc, char **argv) {
    const char *disk_path = NULL;
    long frames_limit = -1;      // -1 = run until quit
    float scale = 2.0f;
    uint64_t ips = 400000;       // CPU instructions stepped per rendered frame
    for (int i = 1; i + 1 <= argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--boot-rk") == 0) {
            disk_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--frames") == 0) {
            frames_limit = atol(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--scale") == 0) {
            scale = (float)atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--ips") == 0) {
            ips = strtoull(argv[++i], NULL, 10);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    if (!SDL_CreateWindowAndRenderer("PDP-11/70",
                                     (int)((float)WIN_W * scale),
                                     (int)((float)WIN_H * scale), 0, &win,
                                     &ren)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_SetRenderScale(ren, scale, scale);
    SDL_StartTextInput(win);

    pdp11_cpu *cpu = pdp11_cpu_create();
    vt_term term;
    vt_init(&term);
    uint16_t *disk = NULL;
    if (cpu == NULL) {
        SDL_Quit();
        return 2;
    }
    pdp11_console_set_sink(cpu, console_sink, &term);

    if (disk_path != NULL) {
        uint32_t words = 0;
        disk = load_disk(disk_path, &words);
        if (disk == NULL) {
            pdp11_cpu_destroy(cpu);
            SDL_Quit();
            return 2;
        }
        pdp11_rk_attach(cpu, disk, words);
        deposit(cpu, BOOT_START, rk_boot_rom,
                sizeof rk_boot_rom / sizeof rk_boot_rom[0]);
        cpu->r[PDP11_PC] = (uint16_t)(BOOT_START + 2u);
    } else {
        deposit(cpu, DEMO_START, demo_rom,
                sizeof demo_rom / sizeof demo_rom[0]);
        for (size_t i = 0; demo_msg[i] != '\0'; ++i) {
            pdp11_mem_write_byte(cpu->mem, 01040u + (uint32_t)i,
                                 (uint8_t)demo_msg[i]);
        }
        cpu->r[PDP11_PC] = DEMO_START;
    }

    bool running = true;
    long frame = 0;
    uint64_t total = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                int b = key_to_byte(ev.key.key);
                if (b >= 0 && !(cpu->tti_csr & DL11_DONE)) {
                    pdp11_console_input(cpu, (uint8_t)b);
                }
            } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                for (const char *p = ev.text.text; *p != '\0'; ++p) {
                    if (!(cpu->tti_csr & DL11_DONE)) {
                        pdp11_console_input(cpu, (uint8_t)*p);
                    }
                }
            }
        }

        for (uint64_t k = 0; k < ips && !cpu->halted; ++k) {
            pdp11_cpu_step(cpu);
            ++total;
        }

        render(ren, &term, cpu, total);

        if (frames_limit >= 0 && ++frame >= frames_limit) {
            running = false;
        } else if (frames_limit < 0) {
            SDL_Delay(16); // ~60 fps when interactive
        }
    }

    pdp11_cpu_destroy(cpu);
    free(disk);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
