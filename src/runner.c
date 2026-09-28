#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <termios.h>

#include "emulator/emulator.h"

#define KEY_QUIT 'q'

static volatile sig_atomic_t g_quit = 0;

/* ---- terminal input ------------------------------------------------------ */

static struct termios term_old;
static int term_raw_on = 0;
static int input_available = 0;

static void term_raw_enter(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &term_old) != 0) return;
    struct termios raw = term_old;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    term_raw_on = 1;
    input_available = 1;
}

static void term_raw_exit(void) {
    if (term_raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &term_old);
        term_raw_on = 0;
    }
}

/* Drain pending key presses into the hardware keypad state. */
static void input_poll(HW *hw) {
    uint16_t keys;
    int c;

    if (!input_available) return;

    keys = 0xFFFF; /* active low: a cleared bit means pressed */
    while ((c = getchar()) != EOF) {
        if (c == 0x1B) {
            int a = getchar();
            if (a != '[') {
                g_quit = 1;
                return;
            }
            switch (getchar()) {
                case 'A': keys &= (uint16_t)~HW_KEY_UP; break;
                case 'B': keys &= (uint16_t)~HW_KEY_DOWN; break;
                case 'C': keys &= (uint16_t)~HW_KEY_RIGHT; break;
                case 'D': keys &= (uint16_t)~HW_KEY_LEFT; break;
                default: break;
            }
            continue;
        }
        switch (c) {
            case KEY_QUIT:
            case 0x03: /* ^C */
                g_quit = 1;
                return;
            case '\n':
            case '\r':
            case ' ':
                keys &= (uint16_t)~HW_KEY_START;
                break;
            case 127:
            case 8:
                keys &= (uint16_t)~HW_KEY_SELECT;
                break;
            case 'z':
            case 'Z':
                keys &= (uint16_t)~HW_KEY_B;
                break;
            case 'x':
            case 'X':
                keys &= (uint16_t)~HW_KEY_A;
                break;
            case 'a':
            case 'A':
                keys &= (uint16_t)~HW_KEY_L;
                break;
            case 's':
            case 'S':
                keys &= (uint16_t)~HW_KEY_R;
                break;
            default:
                break;
        }
    }
    /* KEYINPUT is active low: `keys` already has a cleared bit per pressed
     * key, which is exactly the register value. */
    hw_set_keys(hw, 0x03FF, keys);
}

/* ---- terminal preview ---------------------------------------------------- */

/* Smallest integer subsampling factor that fits the preview in the terminal. */
static int detect_scale(void) {
    struct winsize ws;
    int sx = 1, sy = 1;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        while (PPU_W / sx > (int)ws.ws_col) sx++;
        while (PPU_H / (2 * sy) > (int)ws.ws_row) sy++;
    }
    return sx > sy ? sx : sy;
}

static char *put_pixel(char *p, unsigned v) {
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10)  *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

/* Draw the frame as half-block cells (two scanlines per character row). */
static void render_ansi(const uint32_t *pixels, int scale) {
    enum { CELL_BYTES = 40 };
    char *row = malloc((size_t)(PPU_W / scale) * CELL_BYTES + 8);
    int rows = PPU_H / (2 * scale);

    if (!row) return;

    fputs("\x1b[H", stdout);
    for (int cy = 0; cy < rows; cy++) {
        char *p = row;
        for (int cx = 0; cx < PPU_W / scale; cx++) {
            int x = cx * scale;
            uint32_t top = pixels[(cy * 2 * scale) * PPU_W + x];
            uint32_t bot = pixels[(cy * 2 * scale + scale) * PPU_W + x];

            memcpy(p, "\x1b[38;2;", 7); p += 7;
            p = put_pixel(p, (top >> 16) & 0xFF);
            *p++ = ';';
            p = put_pixel(p, (top >> 8) & 0xFF);
            *p++ = ';';
            p = put_pixel(p, top & 0xFF);
            memcpy(p, "m\x1b[48;2;", 7); p += 7;
            p = put_pixel(p, (bot >> 16) & 0xFF);
            *p++ = ';';
            p = put_pixel(p, (bot >> 8) & 0xFF);
            *p++ = ';';
            p = put_pixel(p, bot & 0xFF);
            *p++ = 'm';
            memcpy(p, "\xe2\x96\x80", 3); /* U+2580 upper half block */
            p += 3;
        }
        *p++ = '\x1b';
        *p++ = '[';
        *p++ = '0';
        *p++ = 'm';
        *p++ = '\n';
        fwrite(row, 1, (size_t)(p - row), stdout);
    }
    fflush(stdout);
    free(row);
}

/* ---- image dump ---------------------------------------------------------- */

static int write_ppm(const char *path, const uint32_t *pixels) {
    FILE *f = fopen(path, "wb");
    uint8_t *rgb;

    if (!f) return 0;

    rgb = malloc((size_t)PPU_W * PPU_H * 3);
    if (!rgb) {
        fclose(f);
        return 0;
    }
    for (int i = 0; i < PPU_W * PPU_H; i++) {
        rgb[i * 3 + 0] = (uint8_t)((pixels[i] >> 16) & 0xFF);
        rgb[i * 3 + 1] = (uint8_t)((pixels[i] >> 8) & 0xFF);
        rgb[i * 3 + 2] = (uint8_t)(pixels[i] & 0xFF);
    }

    fprintf(f, "P6\n%d %d\n255\n", PPU_W, PPU_H);
    fwrite(rgb, 1, (size_t)PPU_W * PPU_H * 3, f);
    free(rgb);
    fclose(f);
    return 1;
}

/* ---- CLI ----------------------------------------------------------------- */

static void print_usage(void) {
    printf("usage: gba <rom.gba> [options]\n"
           "\n"
           "options:\n"
           "  --frames N      stop after N frames (default 600)\n"
           "  --bios FILE     use a real 16 KB BIOS instead of the built-in one\n"
           "  --save FILE     cartridge backup (default: <rom>.sav)\n"
           "  --no-save       never read or write the backup file\n"
           "  --dump FILE     write the last frame as a PPM image\n"
           "  --headless      no terminal preview\n"
           "  --scale N       preview subsampling factor (default: auto)\n"
           "  --no-sleep      run as fast as possible, ignoring 60 fps pacing\n"
           "  --no-input      don't read the keyboard\n"
           "  --help          this text\n"
           "\n"
           "controls: arrows = D-pad, Z = B, X = A, A = L, S = R,\n"
           "          Enter = Start, Backspace = Select, Q = quit\n");
}

/* Build "<rom>.sav" next to the ROM. */
static char *save_path_for(const char *rom) {
    size_t n = strlen(rom);
    char *path = malloc(n + 5);

    if (!path) return NULL;
    memcpy(path, rom, n);
    memcpy(path + n, ".sav", 5);
    return path;
}

static void on_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, const char *argv[]) {
    Emulator emu;
    uint32_t frames = 600;
    int      headless = 0;
    int      no_input = 0;
    int      no_save = 0;
    int      no_sleep = 0;
    int      scale = 0;
    const char *dump = NULL;
    const char *bios = NULL;
    const char *save = NULL;
    char     *own_save = NULL;
    const char *rom = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios = argv[++i];
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            save = argv[++i];
        } else if (strcmp(argv[i], "--no-save") == 0) {
            no_save = 1;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
        } else if (strcmp(argv[i], "--no-sleep") == 0) {
            no_sleep = 1;
        } else if (strcmp(argv[i], "--no-input") == 0) {
            no_input = 1;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = (int)strtol(argv[++i], NULL, 10);
            if (scale < 1) scale = 1;
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            rom = argv[i];
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    if (!rom) {
        fprintf(stderr, "error: no ROM file given\n");
        print_usage();
        return 1;
    }
    if (frames == 0) {
        fprintf(stderr, "error: --frames must be at least 1\n");
        return 1;
    }

    emulator_init(&emu);

    if (bios) {
        if (!emulator_load_bios(&emu, bios)) {
            fprintf(stderr, "error: could not load BIOS '%s' "
                            "(expected 16384 bytes)\n", bios);
            emulator_free(&emu);
            return 1;
        }
    }

    if (!emulator_load_rom(&emu, rom)) {
        fprintf(stderr, "error: could not load ROM '%s'\n", rom);
        emulator_free(&emu);
        return 1;
    }

    if (!no_save) {
        if (save) {
            own_save = strdup(save);
        } else {
            own_save = save_path_for(rom);
        }
        if (own_save) {
            if (emulator_load_save(&emu, own_save)) {
                printf("Loaded save '%s'\n", own_save);
            }
        }
    }

    if (!no_input) {
        term_raw_enter();
    }
    atexit(term_raw_exit);
    signal(SIGINT, on_signal);

    printf("Loaded '%s' (%u bytes)\n", rom, emu.mem.rom_size);
    printf("BIOS: %s\n", emu.has_bios ? bios : "built-in (high level emulation)");
    printf("Running %u frame(s), %s input\n\n", frames,
           input_available ? "keyboard" : "none");

    int preview = !headless && isatty(STDOUT_FILENO);
    if (preview) {
        if (scale == 0) scale = detect_scale();
        fputs("\x1b[2J", stdout);
    }

    double start = now_seconds();
    double next = start;
    uint32_t f = 0;
    uint32_t stuck_frames = 0;
    uint32_t last_pc = emu.cpu.reg[15];

    for (f = 0; f < frames && !g_quit; f++) {
        emulator_frame(&emu);

        input_poll(&emu.hw);

        /* A game that spins on one address is almost always a broken jump
         * into unmapped memory; say so instead of pretending all is well. */
        if (emu.cpu.reg[15] == last_pc) {
            if (++stuck_frames == 300) {
                printf("\nWarning: PC has not moved for 300 frames "
                       "(stuck at 0x%08X)\n", last_pc);
            }
        } else {
            stuck_frames = 0;
            last_pc = emu.cpu.reg[15];
        }

        if (preview) {
            render_ansi(emu.ppu.frame, scale);
        }

        if (!no_sleep) {
            next += 1.0 / 60.0;
            double delay = next - now_seconds();
            if (delay < 0) {
                next = now_seconds();
            } else {
                struct timespec ts;
                ts.tv_sec = (time_t)delay;
                ts.tv_nsec = (long)((delay - (double)ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            }
        }
    }

    double elapsed = now_seconds() - start;
    if (elapsed < 1e-6) elapsed = 1e-6;

    if (dump) {
        if (!write_ppm(dump, emu.ppu.frame)) {
            fprintf(stderr, "error: could not write '%s'\n", dump);
        } else {
            printf("Wrote the last frame to %s\n", dump);
        }
    }

    if (own_save) {
        if (emulator_store_save(&emu, own_save)) {
            printf("Saved backup to %s\n", own_save);
        } else {
            fprintf(stderr, "warning: could not write '%s'\n", own_save);
        }
        free(own_save);
    }

    printf("Done: %u frame(s) in %.2f s (%.1f fps, %.1fx real time)\n",
           f, elapsed, (double)f / elapsed, (double)f / (elapsed * 60.0));
    emulator_free(&emu);
    return 0;
}
