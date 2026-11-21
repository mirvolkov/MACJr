/*
 * textviewer.c -- TEXTVIEWER.ELF: view a text file given as argv[1]. The
 * navigator opens text files (.txt .md .log .ini .cfg .csv .json .h) with
 * /BIN/TEXTVIEWER.ELF. Built on the board: open this file in the
 * navigator, CC.ELF writes textviewer.elf next to it; copy that to /BIN.
 *
 *   ═════════════[README.TXT]══════════════
 *   Long lines are wrapped at word        ▲
 *   bounds to the window, 39 columns;     ■
 *   the scroll bar on the right shows     ▒
 *   where the window is in the file.      ▼
 *    Line 1/356 0%        -+ page  Cx exit
 *
 * Keys: П→x / x→П a row up / down, − / + a page, ←ШГ / ШГ→ the start /
 * the end of the file, Cx exits. From a USB keyboard: arrows, Home, End,
 * Space and b (a page down / up), q.
 *
 * The text is taken as UTF-8 if the whole file is valid UTF-8 -- Cyrillic
 * (Ukrainian too), box drawing and common punctuation get their CP866
 * glyphs, anything else shows as '?' -- otherwise as CP866, byte for byte.
 * Tabs stop every 4 columns, control characters show as '.'. Files up to
 * 512 KB; anything past that is cut.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <macjr.h>

#define TEXT_W     39 /* the last column is the scroll bar */
#define TOP_ROW    2
#define VIEW_H     13 /* rows 2-14 */
#define STATUS_ROW 15
#define TAB        4
#define MAX_BYTES  (512 * 1024)
#define CHUNK      4096
#define CUT_NOTE   "\n[cut at 512 KB]"
#define TITLE_MAX  30

/* the scroll bar, as the firmware's list widget draws it */
#define BAR_UP     0x1E /* ▲ */
#define BAR_DOWN   0x1F /* ▼ */
#define BAR_TRACK  0xB1 /* ▒ */
#define BAR_THUMB  0xFE /* ■ */
#define COLOR_BAR_CELL  "\x1b[0;30;107m"
#define COLOR_BAR_TRACK "\x1b[0;97;40m"
#define COLOR_TEXT      "\x1b[0;37m"

#define NEWLINE (-1) /* decode(): end of a line */

static macjr_tty_t *s_tty;
static unsigned char *s_text; /* the file, s_len bytes */
static int s_len;
static int s_start;           /* past a UTF-8 byte order mark */
static int s_utf8;
static int *s_rows;           /* where each screen row starts in s_text */
static int s_nrows, s_cap;
static int s_top;             /* first row in the window */
static int s_line_row = 0, s_line = 1; /* last line_of() answer */

static void out(const char *s)
{
    macjr_tty_write(s_tty, s, strlen(s));
}

/* ---- characters ---------------------------------------------------- */

/* Unicode -> CP866 beyond the Russian letters. */
static const int s_cp866[][2] = {
    {0x404, 0xF2}, {0x454, 0xF3}, {0x407, 0xF4}, {0x457, 0xF5}, /* Є є Ї ї */
    {0x40E, 0xF6}, {0x45E, 0xF7}, {0x406, 'I'}, {0x456, 'i'},   /* Ў ў І і */
    {0x490, 0x83}, {0x491, 0xA3},                               /* Ґ ґ as Г г */
    {0xB0, 0xF8}, {0x2219, 0xF9}, {0x2022, 0xF9}, {0xB7, 0xFA}, {0x221A, 0xFB},
    {0x2116, 0xFC}, {0xA4, 0xFD}, {0x25A0, 0xFE}, {0xA0, ' '},
    {0x2018, '\''}, {0x2019, '\''}, {0x201C, '"'}, {0x201D, '"'}, {0xAB, '"'},
    {0xBB, '"'}, {0x2013, '-'}, {0x2014, '-'}, {0x2026, '.'}, {0xA9, 'c'},
    {0x2591, 0xB0}, {0x2592, 0xB1}, {0x2593, 0xB2}, {0x2502, 0xB3}, {0x2524, 0xB4},
    {0x2561, 0xB5}, {0x2562, 0xB6}, {0x2556, 0xB7}, {0x2555, 0xB8}, {0x2563, 0xB9},
    {0x2551, 0xBA}, {0x2557, 0xBB}, {0x255D, 0xBC}, {0x255C, 0xBD}, {0x255B, 0xBE},
    {0x2510, 0xBF}, {0x2514, 0xC0}, {0x2534, 0xC1}, {0x252C, 0xC2}, {0x251C, 0xC3},
    {0x2500, 0xC4}, {0x253C, 0xC5}, {0x255E, 0xC6}, {0x255F, 0xC7}, {0x255A, 0xC8},
    {0x2554, 0xC9}, {0x2569, 0xCA}, {0x2566, 0xCB}, {0x2560, 0xCC}, {0x2550, 0xCD},
    {0x256C, 0xCE}, {0x2567, 0xCF}, {0x2568, 0xD0}, {0x2564, 0xD1}, {0x2565, 0xD2},
    {0x2559, 0xD3}, {0x2558, 0xD4}, {0x2552, 0xD5}, {0x2553, 0xD6}, {0x256B, 0xD7},
    {0x256A, 0xD8}, {0x2518, 0xD9}, {0x250C, 0xDA}, {0x2588, 0xDB}, {0x2584, 0xDC},
    {0x258C, 0xDD}, {0x2590, 0xDE}, {0x2580, 0xDF},
};

static int cp866_of(int cp)
{
    int i;
    if (cp >= 0x410 && cp <= 0x43F)
        return 0x80 + cp - 0x410; /* А..п */
    if (cp >= 0x440 && cp <= 0x44F)
        return 0xE0 + cp - 0x440; /* р..я */
    if (cp == 0x401)
        return 0xF0;              /* Ё */
    if (cp == 0x451)
        return 0xF1;              /* ё */
    for (i = 0; i < (int)(sizeof(s_cp866) / sizeof(s_cp866[0])); i++)
        if (s_cp866[i][0] == cp)
            return s_cp866[i][1];
    return '?';
}

/* The UTF-8 sequence at s_text[i]: its length, the code point in *cp; 0
 * if it isn't one. */
static int utf8_seq(int i, int *cp)
{
    int c = s_text[i], n, k, v;
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        v = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        v = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        v = c & 0x07;
    } else {
        return 0;
    }
    if (i + n > s_len)
        return 0;
    for (k = 1; k < n; k++) {
        int b = s_text[i + k];
        if ((b & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (b & 0x3F);
    }
    *cp = v;
    return n;
}

/* UTF-8 if every byte is part of a valid sequence and some aren't ASCII. */
static int is_utf8(void)
{
    int i = s_start, multi = 0, cp, n;
    while (i < s_len) {
        n = utf8_seq(i, &cp);
        if (n == 0)
            return 0;
        if (n > 1)
            multi = 1;
        i += n;
    }
    return multi;
}

/* The character at s_text[i]: its length in bytes; in *glyph the CP866
 * glyph to show, '\t', or NEWLINE (\n, \r\n or \r). */
static int decode(int i, int *glyph)
{
    int c = s_text[i], cp, n;
    if (c == '\n') {
        *glyph = NEWLINE;
        return 1;
    }
    if (c == '\r') {
        *glyph = NEWLINE;
        return i + 1 < s_len && s_text[i + 1] == '\n' ? 2 : 1;
    }
    if (c == '\t') {
        *glyph = '\t';
        return 1;
    }
    if (c < 0x20 || c == 0x7F) {
        *glyph = '.'; /* ESC among them: never pass it to the tty */
        return 1;
    }
    if (c < 0x80 || !s_utf8) {
        *glyph = c;
        return 1;
    }
    n = utf8_seq(i, &cp);
    *glyph = cp866_of(cp);
    return n;
}

/* ---- layout ---------------------------------------------------------- */

/* Lay out the screen row that starts at `start`: where it ends (the text
 * shown is start..end), and in *next where the next row starts. Lines
 * longer than the window break after the last space or tab that fits, or
 * where the window ends if there is none; a space at the break is dropped. */
static int row_end(int start, int *next)
{
    int i = start, col = 0, brk = -1, g, n, w;
    while (i < s_len) {
        n = decode(i, &g);
        if (g == NEWLINE) {
            *next = i + n;
            return i;
        }
        w = g == '\t' ? TAB - col % TAB : 1;
        if (col + w > TEXT_W) {
            if (g == ' ') {
                *next = i + n;
                return i;
            }
            if (brk > start) {
                *next = brk;
                return brk;
            }
            *next = i;
            return i;
        }
        col += w;
        i += n;
        if (g == ' ' || g == '\t')
            brk = i;
    }
    *next = i;
    return i;
}

static int add_row(int start)
{
    if (s_nrows == s_cap) {
        int cap = s_cap ? s_cap * 2 : 1024;
        int *p = realloc(s_rows, cap * sizeof(int));
        if (!p)
            return 0;
        s_rows = p;
        s_cap = cap;
    }
    s_rows[s_nrows++] = start;
    return 1;
}

static int layout(void)
{
    int start = s_start, next;
    for (;;) {
        if (!add_row(start))
            return s_nrows > 0;
        row_end(start, &next);
        if (next >= s_len)
            return 1;
        start = next;
    }
}

/* Whether row r begins a line of the file (not a wrapped part of one). */
static int starts_line(int r)
{
    int c;
    if (r == 0)
        return 1;
    c = s_text[s_rows[r] - 1];
    return c == '\n' || c == '\r';
}

/* The line of the file row r is on, from 1; counted from the last answer. */
static int line_of(int r)
{
    while (s_line_row < r) {
        s_line_row++;
        if (starts_line(s_line_row))
            s_line++;
    }
    while (s_line_row > r) {
        if (starts_line(s_line_row))
            s_line--;
        s_line_row--;
    }
    return s_line;
}

/* ---- screen ---------------------------------------------------------- */

static int max_top(void)
{
    return s_nrows > VIEW_H ? s_nrows - VIEW_H : 0;
}

/* Row r of the text into buf, as glyphs padded to TEXT_W cells; its length. */
static int put_row(char *buf, int r)
{
    int n = 0, col = 0, i, end, next, g;
    if (r < s_nrows) {
        i = s_rows[r];
        end = row_end(i, &next);
        while (i < end) {
            i += decode(i, &g);
            if (g == '\t') {
                do {
                    buf[n++] = ' ';
                    col++;
                } while (col % TAB && col < TEXT_W);
            } else {
                buf[n++] = (char)g;
                col++;
            }
        }
    }
    while (col < TEXT_W) {
        buf[n++] = ' ';
        col++;
    }
    return n;
}

/* The scroll bar cell of window row y. */
static void bar_cell(int y)
{
    char cell[2];
    int track = VIEW_H - 2, thumb;
    cell[1] = 0;
    if (s_nrows <= VIEW_H) {
        out("\x1b[0m ");
        return;
    }
    thumb = 1 + (s_top * (track - 1) + max_top() / 2) / max_top();
    if (y == 0 || y == VIEW_H - 1) {
        cell[0] = (char)(y == 0 ? BAR_UP : BAR_DOWN);
        out(COLOR_BAR_CELL);
    } else if (y == thumb) {
        cell[0] = (char)BAR_THUMB;
        out(COLOR_BAR_CELL);
    } else {
        cell[0] = (char)BAR_TRACK;
        out(COLOR_BAR_TRACK);
    }
    out(cell);
}

static void draw(void)
{
    char buf[64];
    int y, n, pct, m = max_top();
    for (y = 0; y < VIEW_H; y++) {
        snprintf(buf, sizeof(buf), "\x1b[%d;1H" COLOR_TEXT, TOP_ROW + y);
        out(buf);
        n = put_row(buf, s_top + y);
        macjr_tty_write(s_tty, buf, n);
        bar_cell(y);
    }
    pct = m ? s_top * 100 / m : 100;
    /* 39 cells at most: the last cell of the screen would scroll it */
    snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[0;90m Line %d/%d %d%%", STATUS_ROW, line_of(s_top),
             line_of(s_nrows - 1), pct);
    out(buf);
    snprintf(buf, sizeof(buf), "\x1b[K\x1b[%d;23H-+ page  Cx exit\x1b[0m", STATUS_ROW);
    out(buf);
    macjr_display_flush(s_tty);
}

/* A message instead of the text, until a key. */
static int fail(const char *title, const char *msg, const char *detail)
{
    out("\x1b[0m\x1b[2J\x1b[?25l");
    macjr_tty_header(s_tty, title);
    out("\x1b[3;2H\x1b[93m");
    out(msg);
    if (detail) {
        out("\x1b[4;2H\x1b[97m");
        out(detail);
    }
    out("\x1b[15;1H\x1b[0;90m any key: exit\x1b[0m");
    macjr_display_flush(s_tty);
    sys_getkey();
    return 1;
}

/* ---- the file -------------------------------------------------------- */

/* Read the file into s_text; 0 if it can't be opened or memory runs out. */
static int load(const char *path)
{
    int fd = open(path, O_RDONLY), cap = CHUNK, n, cut = 0;
    char probe;
    unsigned char *p;
    if (fd < 0)
        return 0;
    s_text = malloc(cap + sizeof(CUT_NOTE));
    if (!s_text) {
        close(fd);
        return 0;
    }
    s_len = 0;
    for (;;) {
        if (s_len == cap) {
            if (cap >= MAX_BYTES) {
                cut = read(fd, &probe, 1) == 1;
                break;
            }
            cap = cap * 2 > MAX_BYTES ? MAX_BYTES : cap * 2;
            p = realloc(s_text, cap + sizeof(CUT_NOTE));
            if (!p) {
                cut = 1;
                break;
            }
            s_text = p;
        }
        n = read(fd, s_text + s_len, cap - s_len);
        if (n <= 0)
            break;
        s_len += n;
    }
    close(fd);
    if (cut) {
        memcpy(s_text + s_len, CUT_NOTE, sizeof(CUT_NOTE) - 1);
        s_len += sizeof(CUT_NOTE) - 1;
    }
    if (s_len >= 3 && s_text[0] == 0xEF && s_text[1] == 0xBB && s_text[2] == 0xBF)
        s_start = 3; /* UTF-8 byte order mark */
    s_utf8 = is_utf8();
    return 1;
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    char title[TITLE_MAX + 1];
    const char *name;
    int key, top;

    s_tty = tty;
    if (argc < 2)
        return fail("Text viewer", "No file given.", "Open a text file in the navigator.");
    name = strrchr(argv[1], '/');
    name = name ? name + 1 : argv[1];
    strncpy(title, name, TITLE_MAX);
    title[TITLE_MAX] = 0;
    if (!load(argv[1]))
        return fail("Text viewer", "Cannot read the file:", name);
    if (!layout()) {
        free(s_text);
        return fail("Text viewer", "Out of memory.", name);
    }

    out("\x1b[0m\x1b[2J\x1b[?25l");
    macjr_tty_header(tty, title);
    draw();
    for (;;) {
        key = sys_getkey();
        top = s_top;
        switch (key) {
        case KEY_UP:
            top--;
            break;
        case KEY_DOWN:
            top++;
            break;
        case KEY_MINUS:
        case 'b':
            top -= VIEW_H - 1;
            break;
        case KEY_PLUS:
        case KEY_ENTER:
        case ' ':
            top += VIEW_H - 1;
            break;
        case KEY_LEFT: /* ←ШГ, Home */
            top = 0;
            break;
        case KEY_RIGHT: /* ШГ→, End */
            top = max_top();
            break;
        case KEY_ESC:
        case 'q':
            free(s_rows);
            free(s_text);
            return 0;
        }
        if (top > max_top())
            top = max_top();
        if (top < 0)
            top = 0;
        if (top != s_top) {
            s_top = top;
            draw();
        }
    }
}
