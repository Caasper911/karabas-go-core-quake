/*
 * kg_qhw.c -- the board side of the Quake platform (karabas, KG_HW; as htic/kg_htic_hw.c of Hexen):
 * - kg_wait_pak: a test picture and the log until pak0 is there (Ф1б: the OSD item PAK0, Menu+F1, mounts it:
 *   kg_pak.c);
 * - the console log (last lines; term_puts feeds it) drawn on the screen by kg_exit_screen (Sys_Error,
 *   Sys_Quit) in the 8x8 font with a grey palette;
 * - kg_fps_overlay: the OSD item FPS (MMIO_SW 7..0 = 1), the frames of the last second at the top
 *   right of the frame (palette indices 0 black / 15 white of the Quake palette).
 */

#include <stdint.h>
#include "libc/libc.h"
#undef SDRAM_BASE
#include "mmio.h"    /* src/fw (-I) */
#include "font8.h"   /* src/fw */

#define LOG_LINES 25
#define LOG_COLS 40
static char logbuf[LOG_LINES][LOG_COLS + 1];
static int log_line, log_col;

void kg_log_putc(char c)
{
    if (c == '\n' || log_col == LOG_COLS) {
        log_line = (log_line + 1) % LOG_LINES;
        memset(logbuf[log_line], 0, LOG_COLS + 1);
        log_col = 0;
        if (c == '\n') return;
    }
    if (c >= 32 && c < 127) logbuf[log_line][log_col++] = c;
}

static void draw_char(uint8_t *fb, int x, int y, char c, uint8_t col)
{
    const unsigned char *g = font8[(c >= 32 && c < 127) ? c - 32 : 0];
    int r, b;
    for (r = 0; r < 8; r++)
        for (b = 0; b < 8; b++)
            if (g[r] & (0x80 >> b)) fb[(y + r) * 320 + x + b] = col;
}

static void draw_log(uint8_t *fb, uint8_t col)
{
    int i, k;
    for (i = 0; i < LOG_LINES; i++) {
        const char *s = logbuf[(log_line + 1 + i) % LOG_LINES];
        for (k = 0; s[k]; k++) draw_char(fb, k * 8, i * 8, s[k], col);
    }
}

void kg_exit_screen(int code)
{
    uint8_t *fb = (uint8_t *)FB_BASE;
    int i;
    for (i = 0; i < 256; i++) MMIO_PAL(i) = (i << 16) | (i << 8) | i;
    memset(fb, code ? 0x30 : 0x00, 64000);
    draw_log(fb, 0xff);
    MMIO_FRAME = 0;
}

uint32_t kg_pak_size(int k);

void kg_wait_pak(void)
{
    uint8_t *fb = (uint8_t *)FB_BASE;
    const char *msg = "PAK0? id1/pak0.pak: Menu+F1";
    int x, y, i;
    if (kg_pak_size(0) != 0)
        return;
    for (i = 0; i < 256; i++) MMIO_PAL(i) = ((i & 0xe0) << 16) | ((i & 0x1c) << 11) | ((i & 0x03) << 6);
    for (y = 0; y < 200; y++)
        for (x = 0; x < 320; x++) fb[y * 320 + x] = (uint8_t)((y / 25) * 32 + x / 10);
    for (i = 0; msg[i]; i++) draw_char(fb, 8 * i, 192, msg[i], 0xff);
    draw_log(fb, 0xff);
    MMIO_FRAME = 0;
    while (kg_pak_size(0) == 0) {}
}

void kg_fps_overlay(uint8_t *fb)
{
    static uint32_t t0, n;
    static int fps;
    uint32_t now = MMIO_MS;
    char s[3];
    int x0 = 320 - 2 * 8 - 3, y0 = 1, y, i;
    n++;
    if (now - t0 >= 1000) { fps = (int)((n * 1000 + (now - t0) / 2) / (now - t0)); n = 0; t0 = now; }
    if ((MMIO_SW & 0xff) != 1) return;
    s[0] = fps > 99 ? '9' : '0' + fps / 10 % 10;
    s[1] = fps > 99 ? '9' : '0' + fps % 10;
    s[2] = 0;
    for (y = y0 - 1; y < y0 + 9; y++) memset(fb + y * 320 + x0 - 1, 0, 2 * 8 + 2);
    for (i = 0; i < 2; i++) draw_char(fb, x0 + 8 * i, y0, s[i], 15);
}
