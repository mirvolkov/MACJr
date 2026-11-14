/*
 * TETRIS.ELF -- Tetris in the text console (landscape 40x15). A console app:
 * it draws only through the tty, with CP866 block and box-drawing glyphs and
 * ANSI colour. A 10x13 well on the left (two characters per cell, so the cells
 * look square), a status panel on the right.
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
#include "tty/tty.h"

#define GW 10 /* well columns */
#define GH 13 /* well rows (fits 15 text rows with a border) */

/* CP866 box-drawing + block glyphs */
#define TL "\xC9"       /* ╔ */
#define TR "\xBB"       /* ╗ */
#define BL "\xC8"       /* ╚ */
#define BR "\xBC"       /* ╝ */
#define HZ "\xCD"       /* ═ */
#define VT "\xBA"       /* ║ */
#define BLK "\xDB\xDB"  /* ██ : one cell, two chars wide */

/* ANSI bright-colour codes per piece: I O T S Z J L */
static const char *const PCOLOR[7] = {"96", "93", "95", "92", "91", "94", "97"};

typedef struct {
    int8_t cells[4][2];
    int8_t box;
} piece_t;

static const piece_t PIECES[7] = {
    {{{0, 1}, {1, 1}, {2, 1}, {3, 1}}, 4}, /* I */
    {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}, 2}, /* O */
    {{{1, 0}, {0, 1}, {1, 1}, {2, 1}}, 3}, /* T */
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}}, 3}, /* S */
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}, 3}, /* Z */
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}, 3}, /* J */
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}}, 3}, /* L */
};

static uint8_t s_board[GH][GW]; /* 0 empty, else piece type+1 */
static int s_type, s_rot, s_px, s_py, s_next;
static unsigned s_score, s_lines, s_level;
static bool s_over;
static macjr_tty_t *s_tty;

static uint32_t s_rng = 2463534242u;
static int rnd7(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return (int)(x % 7u);
}

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
            y++;
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

/* --- rendering (tty only) --- */
static char s_buf[4096];
static size_t s_n;

static void put(const char *s) {
    size_t l = strlen(s);
    if (s_n + l < sizeof s_buf) {
        memcpy(s_buf + s_n, s, l);
        s_n += l;
    }
}

static void at(int row, int col) {
    char b[24];
    snprintf(b, sizeof b, "\033[%d;%dH", row, col);
    put(b);
}

static void render(void) {
    uint8_t grid[GH][GW];
    memcpy(grid, s_board, sizeof grid);
    if (!s_over) {
        int c[4][2];
        cells_of(s_type, s_rot, s_px, s_py, c);
        for (int i = 0; i < 4; i++) {
            if (c[i][1] >= 0) {
                grid[c[i][1]][c[i][0]] = (uint8_t)(s_type + 1);
            }
        }
    }

    s_n = 0;
    put("\033[2J");

    /* well: top border, 13 rows, bottom border (ANSI rows 1..15) */
    at(1, 1);
    put(TL);
    for (int i = 0; i < GW * 2; i++) {
        put(HZ);
    }
    put(TR);
    for (int y = 0; y < GH; y++) {
        at(y + 2, 1);
        put(VT);
        for (int x = 0; x < GW; x++) {
            int v = grid[y][x];
            if (v) {
                char col[8];
                snprintf(col, sizeof col, "\033[%sm", PCOLOR[v - 1]);
                put(col);
                put(BLK);
                put("\033[0m");
            } else {
                put("  ");
            }
        }
        put(VT);
    }
    at(GH + 2, 1);
    put(BL);
    for (int i = 0; i < GW * 2; i++) {
        put(HZ);
    }
    put(BR);

    /* status panel, right of the well (well ends at column 22) */
    char b[24];
    at(1, 25);
    put("\033[97mTETRIS\033[0m");
    at(3, 25);
    put("NEXT");
    int nc[4][2];
    cells_of(s_next, 0, 0, 0, nc);
    for (int i = 0; i < 4; i++) {
        at(5 + nc[i][1], 26 + nc[i][0] * 2);
        snprintf(b, sizeof b, "\033[%sm" BLK "\033[0m", PCOLOR[s_next]);
        put(b);
    }
    at(9, 25);
    put("\033[94mSCORE\033[0m");
    at(10, 25);
    snprintf(b, sizeof b, "%u", s_score);
    put(b);
    at(11, 25);
    snprintf(b, sizeof b, "\033[94mLINES\033[0m %u", s_lines);
    put(b);
    at(12, 25);
    snprintf(b, sizeof b, "\033[94mLEVEL\033[0m %u", s_level);
    put(b);
    at(14, 25);
    put("4 6 2 5");
    at(15, 25);
    put("Cx exit");

    if (s_over) {
        at(6, 3);
        put("\033[91m" " GAME OVER " "\033[0m");
        at(8, 3);
        put("\033[97m" "  \x18 again  " "\033[0m");
    }

    macjr_tty_write(s_tty, s_buf, s_n);
    macjr_display_flush(s_tty);
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

static void step_down(void) {
    if (!collides(s_type, s_rot, s_px, s_py + 1)) {
        s_py++;
    } else {
        lock_piece();
    }
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
    s_tty = tty;
    s_rng ^= sys_millis() * 2654435761u;
    if (!s_rng) {
        s_rng = 1;
    }

    int fd = open(KBD_DEV_PATH, O_RDONLY);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }

    reset();
    render();
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
            case KEY_MK_2: step_down(); last = sys_millis(); dirty = true; break;
            case KEY_MK_ENTER: hard_drop(); last = sys_millis(); dirty = true; break;
            default: break;
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
            render();
        }
        sys_delay(10);
    }

done:
    if (fd >= 0) {
        close(fd);
    }
    return 0;
}
