/*
 * TETRIS.ELF -- Tetris on the full 320x240 panel (landscape). A 10x20 well on
 * the left, a side panel (next piece, score, lines, level) on the right. Draws
 * on the display HAL directly (an RGB framebuffer blitted in one call, like the
 * Life and SDR apps), not through the tty.
 *
 * Keys (MK-52 digits): 4 left, 6 right, 2 soft-drop, 5 rotate; В↑ hard-drop,
 * Cx exit. After a game over, В↑ starts again.
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "macjr.h"
#include "keyboard.h"
#include "monitor.h"

#define SW 320
#define SH 240

#define GW   10              /* well columns */
#define GH   20              /* well rows */
#define CELL 11              /* px per cell */
#define WX   12              /* well top-left x */
#define WY   10              /* well top-left y */

#define PANEL_X 150

#define BG_RGB 0x0a0a12u

static uint8_t *s_fb;        /* SW*SH*3, PSRAM */

/* --- framebuffer primitives --- */
static void put(int x, int y, uint32_t rgb) {
    if ((unsigned)x >= SW || (unsigned)y >= SH) {
        return;
    }
    uint8_t *p = s_fb + ((size_t)y * SW + x) * 3;
    p[0] = (rgb >> 16) & 0xff;
    p[1] = (rgb >> 8) & 0xff;
    p[2] = rgb & 0xff;
}

static void fill(int x, int y, int w, int h, uint32_t rgb) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put(x + i, y + j, rgb);
        }
    }
}

static void frame(int x, int y, int w, int h, uint32_t rgb) {
    fill(x, y, w, 1, rgb);
    fill(x, y + h - 1, w, 1, rgb);
    fill(x, y, 1, h, rgb);
    fill(x + w - 1, y, 1, h, rgb);
}

static void glyph(int x, int y, unsigned char c, uint32_t fg) {
    const unsigned char *g = macjr_font_builtin_8x16[c];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80u >> col)) {
                put(x + col, y + row, fg);
            }
        }
    }
}

static void text(int x, int y, const char *s, uint32_t fg) {
    for (; *s; s++, x += 8) {
        glyph(x, y, (unsigned char)*s, fg);
    }
}

/* --- pieces --- */
typedef struct {
    int8_t cells[4][2];
    int8_t box;
    uint32_t color;
} piece_t;

static const piece_t PIECES[7] = {
    {{{0, 1}, {1, 1}, {2, 1}, {3, 1}}, 4, 0x25d7d7u}, /* I cyan   */
    {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}, 2, 0xe0c020u}, /* O yellow */
    {{{1, 0}, {0, 1}, {1, 1}, {2, 1}}, 3, 0xb030d0u}, /* T purple */
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}}, 3, 0x40c040u}, /* S green  */
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}, 3, 0xd03030u}, /* Z red    */
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}, 3, 0x4060e0u}, /* J blue   */
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}}, 3, 0xe08020u}, /* L orange */
};

static uint8_t s_board[GH][GW]; /* 0 empty, else piece type+1 */
static int s_type, s_rot, s_px, s_py, s_next;
static unsigned s_score, s_lines, s_level;
static bool s_over;

static uint32_t s_rng = 2463534242u;
static int rnd7(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return (int)(x % 7u);
}

/* The four board cells of piece `type` at rotation `rot`, origin (px,py). */
static void cells_of(int type, int rot, int px, int py, int out[4][2]) {
    int b = PIECES[type].box;
    for (int i = 0; i < 4; i++) {
        int x = PIECES[type].cells[i][0];
        int y = PIECES[type].cells[i][1];
        for (int r = 0; r < rot; r++) {
            int nx = b - 1 - y, ny = x;
            x = nx;
            y = ny;
        }
        out[i][0] = px + x;
        out[i][1] = py + y;
    }
}

static bool collides(int type, int rot, int px, int py) {
    int c[4][2];
    cells_of(type, rot, px, py, c);
    for (int i = 0; i < 4; i++) {
        int x = c[i][0], y = c[i][1];
        if (x < 0 || x >= GW || y < 0 || y >= GH || s_board[y][x]) {
            return true;
        }
    }
    return false;
}

static void spawn(void) {
    s_type = s_next;
    s_next = rnd7();
    s_rot = 0;
    s_px = (GW - PIECES[s_type].box) / 2;
    s_py = 0;
    if (collides(s_type, s_rot, s_px, s_py)) {
        s_over = true;
    }
}

static void lock_piece(void) {
    int c[4][2];
    cells_of(s_type, s_rot, s_px, s_py, c);
    for (int i = 0; i < 4; i++) {
        s_board[c[i][1]][c[i][0]] = (uint8_t)(s_type + 1);
    }
    /* clear full rows */
    int cleared = 0;
    for (int y = GH - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < GW; x++) {
            if (!s_board[y][x]) {
                full = false;
                break;
            }
        }
        if (full) {
            for (int yy = y; yy > 0; yy--) {
                memcpy(s_board[yy], s_board[yy - 1], GW);
            }
            memset(s_board[0], 0, GW);
            cleared++;
            y++; /* recheck this row */
        }
    }
    if (cleared) {
        static const unsigned pts[] = {0, 40, 100, 300, 1200};
        s_score += pts[cleared] * (s_level + 1);
        s_lines += (unsigned)cleared;
        s_level = s_lines / 10;
    }
    spawn();
}

static unsigned drop_ms(void) {
    unsigned d = 800 - s_level * 60;
    return d < 120 ? 120 : d;
}

static void reset(void) {
    memset(s_board, 0, sizeof s_board);
    s_score = s_lines = s_level = 0;
    s_over = false;
    s_next = rnd7();
    spawn();
}

/* --- rendering --- */
static void cell_block(int gx, int gy, uint32_t color) {
    fill(WX + gx * CELL, WY + gy * CELL, CELL - 1, CELL - 1, color);
}

static void mini(int type, int ox, int oy) {
    int c[4][2];
    cells_of(type, 0, 0, 0, c);
    for (int i = 0; i < 4; i++) {
        fill(ox + c[i][0] * 9, oy + c[i][1] * 9, 8, 8, PIECES[type].color);
    }
}

static void render(macjr_hal_display_t *d) {
    for (int i = 0; i < SW * SH; i++) {
        s_fb[i * 3] = (BG_RGB >> 16) & 0xff;
        s_fb[i * 3 + 1] = (BG_RGB >> 8) & 0xff;
        s_fb[i * 3 + 2] = BG_RGB & 0xff;
    }

    frame(WX - 2, WY - 2, GW * CELL + 3, GH * CELL + 3, 0x707080u);
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            cell_block(x, y, s_board[y][x] ? PIECES[s_board[y][x] - 1].color : 0x000000u);
        }
    }
    if (!s_over) {
        int c[4][2];
        cells_of(s_type, s_rot, s_px, s_py, c);
        for (int i = 0; i < 4; i++) {
            if (c[i][1] >= 0) {
                cell_block(c[i][0], c[i][1], PIECES[s_type].color);
            }
        }
    }

    char buf[16];
    text(PANEL_X, 12, "NEXT", 0xffffffu);
    mini(s_next, PANEL_X, 32);
    text(PANEL_X, 92, "SCORE", 0xa0a0ffu);
    snprintf(buf, sizeof buf, "%u", s_score);
    text(PANEL_X, 110, buf, 0xffffffu);
    text(PANEL_X, 136, "LINES", 0xa0a0ffu);
    snprintf(buf, sizeof buf, "%u", s_lines);
    text(PANEL_X, 154, buf, 0xffffffu);
    text(PANEL_X, 180, "LEVEL", 0xa0a0ffu);
    snprintf(buf, sizeof buf, "%u", s_level);
    text(PANEL_X, 198, buf, 0xffffffu);
    text(PANEL_X, 222, "4 6 2 5", 0x808080u);

    if (s_over) {
        fill(40, 100, 160, 40, 0x000000u);
        frame(40, 100, 160, 40, 0xd03030u);
        text(60, 108, "GAME OVER", 0xff6060u);
        text(52, 124, "\x18 again  Cx exit", 0xa0a0a0u);
    }

    d->blit_rgb888(d, 0, 0, SW, SH, s_fb);
    if (d->end_frame) {
        d->end_frame(d);
    }
}

/* --- moves --- */
static void move_h(int dx) {
    if (!collides(s_type, s_rot, s_px + dx, s_py)) {
        s_px += dx;
    }
}

static void rotate(void) {
    int nr = (s_rot + 1) & 3;
    static const int kick[] = {0, -1, 1, -2, 2};
    for (size_t i = 0; i < sizeof kick / sizeof kick[0]; i++) {
        if (!collides(s_type, nr, s_px + kick[i], s_py)) {
            s_px += kick[i];
            s_rot = nr;
            return;
        }
    }
}

/* Move down one; lock and spawn if it can't. Returns true if it locked. */
static bool step_down(void) {
    if (!collides(s_type, s_rot, s_px, s_py + 1)) {
        s_py++;
        return false;
    }
    lock_piece();
    return true;
}

static void hard_drop(void) {
    while (!collides(s_type, s_rot, s_px, s_py + 1)) {
        s_py++;
        s_score += 2;
    }
    lock_piece();
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
    (void)argc;
    (void)argv;
    macjr_hal_display_t *d = macjr_tty_display(tty);
    if (!d || !d->blit_rgb888) {
        return 1;
    }
    s_fb = malloc((size_t)SW * SH * 3);
    if (!s_fb) {
        return 1;
    }
    s_rng ^= sys_millis() * 2654435761u;
    if (!s_rng) {
        s_rng = 1;
    }

    int fd = open(KBD_DEV_PATH, O_RDONLY);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }

    reset();
    render(d);
    unsigned last = sys_millis();

    for (;;) {
        bool dirty = false;
        uint8_t key;
        while (fd >= 0 && read(fd, &key, 1) == 1) {
            if (KBD_IS_CHORD(key)) {
                continue;
            }
            if (key == KEY_ESC) {
                goto done;
            }
            if (s_over) {
                if (key == KEY_MK_ENTER) {
                    reset();
                    last = sys_millis();
                    dirty = true;
                }
                continue;
            }
            switch (key) {
            case KEY_MK_4: move_h(-1); dirty = true; break;
            case KEY_MK_6: move_h(1);  dirty = true; break;
            case KEY_MK_5: rotate();   dirty = true; break;
            case KEY_MK_2:
                step_down();
                last = sys_millis();
                dirty = true;
                break;
            case KEY_MK_ENTER:
                hard_drop();
                last = sys_millis();
                dirty = true;
                break;
            default:
                break;
            }
        }

        if (monitor_is_open()) {
            sys_delay(50);
            continue;
        }

        if (!s_over && sys_millis() - last >= drop_ms()) {
            step_down();
            last = sys_millis();
            dirty = true;
        }

        if (dirty) {
            render(d);
        }
        sys_delay(10);
    }

done:
    if (fd >= 0) {
        close(fd);
    }
    free(s_fb);
    return 0;
}
