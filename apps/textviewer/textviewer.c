/*
 * textviewer.c -- TEXTVIEWER.ELF: view a text file given as argv[1]. The
 * navigator opens text files (.txt .md .log .ini .cfg .csv .json .h) with
 * /bin/TEXTVIEWER.ELF. Built by apps/Makefile like the other programs;
 * it also builds on the board with CC.ELF (one file, on-board headers only).
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
 * The file is streamed, the way less does it: only a 16 KB window of it is
 * in memory, read again from the card wherever the view moves, so a file
 * of any size opens at once and takes the same memory. A screen row is
 * known by the file offset it starts at: the next row is laid out forward
 * from there, the previous one from the start of its line. The scroll bar
 * and the percentage come from that offset, not from a count of rows.
 * Lines are counted once, after the first screen is up, for files up to
 * 2 MB; past that the status shows the line only while it is known
 * (from the start, not after a jump to the end).
 *
 * The text is taken as UTF-8 if its first 64 KB are valid UTF-8 -- Cyrillic
 * (Ukrainian too), box drawing and common punctuation get their CP866
 * glyphs, anything else shows as '?' -- otherwise as CP866, byte for byte.
 * Tabs stop every 4 columns, control characters show as '.'.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <macjr.h>

/* The MK-52 keys: the on-board macjr.h names them, the SDK one doesn't. */
#ifndef KEY_UP
#define KEY_ESC   SYS_KEY_ESC
#define KEY_ENTER SYS_KEY_ENTER
#define KEY_PLUS  142 /* + */
#define KEY_MINUS 144 /* − */
#define KEY_UP    156 /* П→x */
#define KEY_DOWN  157 /* x→П */
#define KEY_LEFT  151 /* ←ШГ, Home */
#define KEY_RIGHT 155 /* ШГ→, End */
#endif

#define TEXT_W     39 /* the last column is the scroll bar */
#define TOP_ROW    2
#define VIEW_H     13 /* rows 2-14 */
#define STATUS_ROW 15
#define TAB        4
#define TITLE_MAX  30

#define WIN        (16 * 1024)       /* the part of the file in memory */
#define BACK_MAX   4096              /* how far back a line start is looked for */
#define SNIFF_MAX  (64 * 1024)       /* the part checked for UTF-8 */
#define COUNT_MAX  (2 * 1024 * 1024) /* files whose lines are counted */

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
static int s_fd = -1;
static long s_size;           /* of the file */
static long s_start;          /* past a UTF-8 byte order mark */
static int s_utf8;
static unsigned char *s_buf;  /* bytes s_base.. s_base + s_blen of the file */
static long s_base, s_blen;
static long s_top;            /* where the first row of the window starts */
static long s_max_top;        /* ... with the last row of the file at the bottom */
static int s_line;            /* the line s_top is on, from 1; 0 if not known */
static int s_lines;           /* lines in the file; 0 if not counted */
static int s_max_back;        /* lines begun between s_max_top and the last row */

static void out(const char *s)
{
    macjr_tty_write(s_tty, s, strlen(s));
}

/* ---- the file -------------------------------------------------------- */

/* Read the window around `off` (more of it before than after: the view
 * moves both ways). */
static void fill(long off)
{
    long base = off - WIN / 2;
    int n;
    if (base < 0)
        base = 0;
    base &= ~511L; /* whole card sectors */
    s_base = base;
    s_blen = 0;
    if (lseek(s_fd, base, SEEK_SET) != base)
        return;
    while (s_blen < WIN) {
        n = read(s_fd, s_buf + s_blen, WIN - s_blen);
        if (n <= 0)
            break;
        s_blen += n;
    }
}

/* The byte at `off`, -1 past the end (or if the card fails). */
static int byte_at(long off)
{
    if (off < 0 || off >= s_size)
        return -1;
    if (off < s_base || off >= s_base + s_blen) {
        fill(off);
        if (off < s_base || off >= s_base + s_blen)
            return -1;
    }
    return s_buf[off - s_base];
}

static int is_newline(int c)
{
    return c == '\n' || c == '\r';
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

/* The UTF-8 sequence at `off`: its length, the code point in *cp; 0 if it
 * isn't one. */
static int utf8_seq(long off, int *cp)
{
    int c = byte_at(off), n, k, v, b;
    if (c < 0)
        return 0;
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
    for (k = 1; k < n; k++) {
        b = byte_at(off + k);
        if (b < 0 || (b & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (b & 0x3F);
    }
    *cp = v;
    return n;
}

/* UTF-8 if the first SNIFF_MAX bytes are valid sequences and some aren't
 * ASCII. */
static int sniff_utf8(void)
{
    long i = s_start, end = s_start + SNIFF_MAX;
    int multi = 0, cp, n;
    if (end > s_size)
        end = s_size;
    while (i < end) {
        n = utf8_seq(i, &cp);
        if (n == 0)
            return 0;
        if (n > 1)
            multi = 1;
        i += n;
    }
    return multi;
}

/* The character at `off`: its length in bytes; in *glyph the CP866 glyph
 * to show, '\t', or NEWLINE (\n, \r\n or \r). */
static int decode(long off, int *glyph)
{
    int c = byte_at(off), cp, n;
    if (c == '\n') {
        *glyph = NEWLINE;
        return 1;
    }
    if (c == '\r') {
        *glyph = NEWLINE;
        return byte_at(off + 1) == '\n' ? 2 : 1;
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
    n = utf8_seq(off, &cp);
    if (n == 0) {
        *glyph = '?'; /* broken UTF-8 past the part checked */
        return 1;
    }
    *glyph = cp866_of(cp);
    return n;
}

/* ---- rows -------------------------------------------------------------- */

/* Lay out the screen row that starts at `start`: where it ends (the text
 * shown is start..end), and in *next where the next row starts (s_size
 * if it is the last). Lines longer than the window break after the last
 * space or tab that fits, or where the window ends if there is none; a
 * space at the break is dropped. */
static long row_end(long start, long *next)
{
    long i = start, brk = -1;
    int col = 0, g, n, w;
    while (i < s_size) {
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

/* Whether the row at `off` begins a line of the file (not a wrapped part
 * of one). */
static int starts_line(long off)
{
    return off <= s_start || is_newline(byte_at(off - 1));
}

/* Where the row before the one at `start` starts: lay out its line from
 * the line's start. A line longer than BACK_MAX is laid out from BACK_MAX
 * back instead, so rows of such a line may break differently going up
 * than going down. */
static long prev_row(long start)
{
    long p = start, lim, r, next;
    if (start <= s_start)
        return s_start;
    if (byte_at(p - 1) == '\n') { /* step over the end of the line before */
        p--;
        if (p > s_start && byte_at(p - 1) == '\r')
            p--;
    } else if (byte_at(p - 1) == '\r') {
        p--;
    }
    lim = p - BACK_MAX;
    if (lim < s_start)
        lim = s_start;
    while (p > lim && !is_newline(byte_at(p - 1)))
        p--;
    for (r = p;; r = next) {
        row_end(r, &next);
        if (next >= start || next <= r)
            return r;
    }
}

/* Lines in the file, if it is small enough to count them; 0 if not. A line
 * ends with \n, \r\n or \r; a last line without one counts too. */
static int count_lines(void)
{
    long off;
    int i, n = 0, prev = 0, c;
    if (s_size > COUNT_MAX)
        return 0;
    for (off = s_start; off < s_size; off = s_base + s_blen) {
        fill(off + WIN / 2); /* the window starting about at off */
        if (s_blen == 0 || s_base + s_blen <= off)
            return 0;
        for (i = (int)(off - s_base); i < s_blen; i++) {
            c = s_buf[i];
            if (c == '\r' || (c == '\n' && prev != '\r'))
                n++;
            prev = c;
        }
    }
    if (s_size == s_start || !is_newline(byte_at(s_size - 1)))
        n++;
    return n;
}

/* ---- moving ---------------------------------------------------------- */

static void row_up(void)
{
    if (s_top <= s_start)
        return;
    if (s_line && starts_line(s_top))
        s_line--;
    s_top = prev_row(s_top);
}

static void row_down(void)
{
    long next;
    if (s_top >= s_max_top)
        return;
    row_end(s_top, &next);
    if (next > s_max_top) { /* rows that broke differently going up */
        s_top = s_max_top;
        s_line = s_lines ? s_lines - s_max_back : 0;
        return;
    }
    if (s_line && starts_line(next))
        s_line++;
    s_top = next;
}

/* The last screen: s_max_top, and how many lines begin below it. */
static void find_end(void)
{
    int y;
    s_max_top = s_size > s_start ? prev_row(s_size) : s_start;
    s_max_back = 0;
    for (y = 1; y < VIEW_H && s_max_top > s_start; y++) {
        if (starts_line(s_max_top))
            s_max_back++;
        s_max_top = prev_row(s_max_top);
    }
}

/* ---- screen ---------------------------------------------------------- */

/* a * k / b for 0 <= a <= b, without overflow; rounded or not */
static int scale(long a, long b, int k, int round)
{
    while (b > 0x7FFFFF) {
        a >>= 1;
        b >>= 1;
    }
    if (b == 0)
        return k;
    return (int)((a * k + (round ? b / 2 : 0)) / b);
}

/* The row at `start` into buf, as glyphs padded to TEXT_W cells, or a
 * blank row if start < 0; its length, and in *next where the next row
 * starts. */
static int put_row(char *buf, long start, long *next)
{
    int n = 0, col = 0, g;
    long i = start, end;
    if (start >= 0) {
        end = row_end(i, next);
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
    if (s_max_top <= s_start) {
        out("\x1b[0m ");
        return;
    }
    thumb = 1 + scale(s_top - s_start, s_max_top - s_start, track - 1, 1);
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

static void draw_status(void)
{
    char buf[64], left[40];
    int pct, len;
    pct = s_max_top > s_start ? scale(s_top - s_start, s_max_top - s_start, 100, 0) : 100;
    if (s_line && s_lines)
        snprintf(left, sizeof(left), " Line %d/%d %d%%", s_line, s_lines, pct);
    else if (s_line)
        snprintf(left, sizeof(left), " Line %d %d%%", s_line, pct);
    else
        snprintf(left, sizeof(left), " %d%%", pct);
    snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[0;90m", STATUS_ROW);
    out(buf);
    out(left);
    out("\x1b[K");
    /* 39 cells at most: the last cell of the screen would scroll it */
    len = strlen(left);
    if (len <= 21)
        snprintf(buf, sizeof(buf), "\x1b[%d;23H-+ page  Cx exit", STATUS_ROW);
    else if (len <= 30)
        snprintf(buf, sizeof(buf), "\x1b[%d;32HCx exit", STATUS_ROW);
    else
        buf[0] = 0;
    out(buf);
    out("\x1b[0m");
}

static void draw(void)
{
    char buf[64];
    int y, n, more = 1;
    long r = s_top, next = s_size;
    for (y = 0; y < VIEW_H; y++) {
        snprintf(buf, sizeof(buf), "\x1b[%d;1H" COLOR_TEXT, TOP_ROW + y);
        out(buf);
        n = put_row(buf, more ? r : -1, &next);
        macjr_tty_write(s_tty, buf, n);
        bar_cell(y);
        if (more) {
            more = next < s_size;
            r = next;
        }
    }
    draw_status();
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

static void finish(void)
{
    if (s_fd >= 0)
        close(s_fd);
    free(s_buf);
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    char title[TITLE_MAX + 1];
    const char *name;
    int key, n, line;
    long top;

    s_tty = tty;
    if (argc < 2)
        return fail("Text viewer", "No file given.", "Open a text file in the navigator.");
    name = strrchr(argv[1], '/');
    name = name ? name + 1 : argv[1];
    strncpy(title, name, TITLE_MAX);
    title[TITLE_MAX] = 0;
    s_fd = open(argv[1], O_RDONLY);
    s_size = s_fd < 0 ? -1 : lseek(s_fd, 0, SEEK_END);
    if (s_size < 0) {
        finish();
        return fail("Text viewer", "Cannot read the file:", name);
    }
    s_buf = malloc(WIN);
    if (!s_buf) {
        finish();
        return fail("Text viewer", "Out of memory.", name);
    }
    s_blen = 0;
    if (byte_at(0) == 0xEF && byte_at(1) == 0xBB && byte_at(2) == 0xBF)
        s_start = 3; /* UTF-8 byte order mark */
    s_utf8 = sniff_utf8();
    find_end();
    s_top = s_start;
    s_line = 1;

    out("\x1b[0m\x1b[2J\x1b[?25l");
    macjr_tty_header(tty, title);
    draw();
    s_lines = count_lines(); /* with the first screen up already */
    if (s_lines) {
        draw_status();
        macjr_display_flush(tty);
    }
    for (;;) {
        key = sys_getkey();
        top = s_top;
        line = s_line;
        switch (key) {
        case KEY_UP:
            row_up();
            break;
        case KEY_DOWN:
            row_down();
            break;
        case KEY_MINUS:
        case 'b':
            for (n = 0; n < VIEW_H - 1; n++)
                row_up();
            break;
        case KEY_PLUS:
        case KEY_ENTER:
        case ' ':
            for (n = 0; n < VIEW_H - 1; n++)
                row_down();
            break;
        case KEY_LEFT: /* ←ШГ, Home */
            s_top = s_start;
            s_line = 1;
            break;
        case KEY_RIGHT: /* ШГ→, End */
            s_top = s_max_top;
            s_line = s_lines ? s_lines - s_max_back : 0;
            break;
        case KEY_ESC:
        case 'q':
            finish();
            return 0;
        }
        if (s_top != top || s_line != line)
            draw();
    }
}
