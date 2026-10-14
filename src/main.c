/*
 * main.c -- command line front end for the Game Boy emulator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"
#include "frontend.h"

static void print_usage(const char *argv0)
{
    printf("Usage: %s [options] <rom>\n"
           "\n"
           "A Game Boy (DMG) emulator written in C.\n"
           "\n"
           "Options:\n"
           "  -f, --frames N     run N frames and exit (0 = run until quit)\n"
           "  -s, --screenshot P  write the final frame to a BMP file\n"
           "      --sound MODE   none | auto | \"player command\"\n"
           "      --ascii        use plain ASCII glyphs instead of blocks\n"
           "      --no-color     disable 24 bit colour output\n"
           "      --mono         force the monochrome density glyphs\n"
           "  -q, --quiet        do not print cartridge information\n"
           "\n"
           "Display:\n"
           "      --size WxH     pretend the terminal is W columns by H rows\n"
           "      --aspect N     character cell height as a multiple of its width\n"
           "      --no-resize    never try to grow a window too small for the\n"
           "                     picture (Windows consoles are resized by\n"
           "                     default; a Unix terminal always is not)\n"
           "\n"
           "  -h, --help         show this help\n"
           "  -V, --version      show the version\n"
           "\n"
           "Controls: arrows or WASD move, Z is A, X is B, Enter is Start,\n"
           "Space is Select, P pauses, R resets, M mutes, ? lists the keys,\n"
           "Q quits.\n",
           argv0);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Parses "COLSxROWS", or either half on its own as "80x" or "x24". */
static bool parse_size(const char *text, int *cols, int *rows)
{
    const char *x = strchr(text, 'x');
    char *end;

    if (!x)
        return false;

    if (x != text) {
        long v = strtol(text, &end, 10);
        if (end != x || v < 1 || v > 10000)
            return false;
        *cols = (int)v;
    }
    if (x[1] != '\0') {
        long v = strtol(x + 1, &end, 10);
        if (*end != '\0' || v < 1 || v > 10000)
            return false;
        *rows = (int)v;
    }
    return true;
}

int main(int argc, char **argv)
{
    FrontendOptions opt;
    Cart cart;
    GB gb;
    const char *rom_path = NULL;
    char err[256];
    bool quiet = false;
    int i;

    memset(&opt, 0, sizeof(opt));
    opt.sound = "none";

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("gbcpu 1.0 -- a Game Boy emulator in C\n");
            return 0;
        }
        if (strcmp(a, "-f") == 0 || strcmp(a, "--frames") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: %s needs a value\n", a);
                return 2;
            }
            opt.frames = strtol(argv[i], NULL, 10);
            continue;
        }
        if (strcmp(a, "-s") == 0 || strcmp(a, "--screenshot") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: %s needs a value\n", a);
                return 2;
            }
            opt.shot = argv[i];
            continue;
        }
        if (strcmp(a, "--sound") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: %s needs a value\n", a);
                return 2;
            }
            opt.sound = argv[i];
            continue;
        }
        if (strcmp(a, "--ascii") == 0) {
            /* Plain glyphs without colour is the one combination that
             * makes sense, so this implies both. */
            opt.ascii = true;
            opt.no_color = true;
            continue;
        }
        if (strcmp(a, "--mono") == 0) {
            opt.no_color = true;
            continue;
        }
        if (strcmp(a, "--no-color") == 0) {
            opt.no_color = true;
            continue;
        }
        if (strcmp(a, "--size") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: %s needs a value\n", a);
                return 2;
            }
            if (!parse_size(argv[i], &opt.force_cols, &opt.force_rows)) {
                fprintf(stderr,
                        "error: --size wants COLUMNSxROWS, for example 100x40\n");
                return 2;
            }
            continue;
        }
        if (strcmp(a, "--aspect") == 0) {
            char *end;
            double v;

            if (++i >= argc) {
                fprintf(stderr, "error: %s needs a value\n", a);
                return 2;
            }
            v = strtod(argv[i], &end);
            if (*end != '\0' || !(v > 0.0) || v > 10.0) {
                fprintf(stderr,
                        "error: --aspect wants a positive number, the cell "
                        "height divided by its width (2 is the usual one)\n");
                return 2;
            }
            opt.aspect = v;
            continue;
        }
        if (strcmp(a, "--no-resize") == 0) {
            opt.no_resize = true;
            continue;
        }
        if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            quiet = true;
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            print_usage(argv[0]);
            return 2;
        }
        rom_path = a;
    }

    if (!rom_path) {
        print_usage(argv[0]);
        return 2;
    }

    if (!gb_load_rom(rom_path, &cart, err, sizeof(err))) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    if (cart.is_color) {
        fprintf(stderr,
                "warning: '%s' is a Game Boy Color cartridge; this emulator "
                "targets the original DMG and some colour games will not run "
                "correctly.\n",
                base_name(rom_path));
    }
    if (cart.mapper == MAPPER_UNKNOWN) {
        fprintf(stderr,
                "warning: unsupported cartridge type 0x%02X; loading it as a "
                "plain ROM.\n",
                cart.type_code);
    }

    if (!quiet) {
        printf("title   : %s\n", cart.title[0] ? cart.title : "(untitled)");
        printf("mapper  : 0x%02X\n", cart.type_code);
        printf("rom     : %zu KiB\n", cart.rom_size / 1024);
        if (cart.ram_size)
            printf("sram    : %zu KiB%s\n", cart.ram_size / 1024,
                   cart.has_battery ? " (battery backed)" : "");
        if (cart.has_rtc)
            printf("rtc     : yes\n");
        fflush(stdout);
    }

    gb_init(&gb, &cart);

    if (frontend_term_run(&gb, &opt) != 0) {
        gb_free_cart(&cart);
        return 1;
    }

    gb_free_cart(&cart);
    return 0;
}