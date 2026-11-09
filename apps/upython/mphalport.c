/*
 * MACJr I/O HAL for the MicroPython embed port.
 *
 * stdout → macjr_tty_write (LCD)
 * stdin  → sys_getkey with MK-52 → ASCII translation
 *
 * Key layout (see components/hal/keyboard/keyboard.h):
 *
 *   Direct:   0-9 → '0'-'9',  . → '.',  + → '+',  − → '-',  × → '*',
 *             ÷ → '/',  ↔ → '=',  /-/ → '(',  ВП → ')',  А↑ → ' ',
 *             В↑ → '\n' (Enter),  Cx → '\b' (backspace)
 *
 *   F-prefix: 1→'''  2→':'  3→','  4→'['  5→']'  6→'{'  7→'}'
 *             8→'"'  9→'_'  0→'%'  .→'!'  В↑→'\t'  ВП→'#'  ↔→'<'
 *             +→'>'  −→'@'  ×→'~'  ÷→'\\' /-/→'^'  А↑→'&'
 *
 *   K-prefix (letters a-z):
 *             1→a  2→b  3→c  4→d  5→e  6→f  7→g  8→h  9→i  0→j
 *             .→k  ↔→l  В↑→m  А↑→n  ВП→o  /-/→p  +→q  -→r
 *             ×→s  ÷→t  ↑↓→u  В/О→v  БП→w  ПП→x  ←ШГ→y  ШГ→→z
 */
#include <stddef.h>
#include <string.h>

#include "py/mphal.h"
#include "macjr.h"
#include "keyboard.h"

static macjr_tty_t *s_tty;

void upy_set_tty(macjr_tty_t *tty) { s_tty = tty; }

/* ---------- stdout ---------- */

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    if (s_tty) {
        macjr_tty_write(s_tty, str, len);
        macjr_display_flush(s_tty);
    }
}

/* ---------- stdin / key translation ---------- */

/* Small ring buffer so a single key press can emit 2-char sequences. */
#define BUF_SIZE 8
static char s_buf[BUF_SIZE];
static int  s_head, s_tail;

static void buf_put(char c) {
    int next = (s_tail + 1) % BUF_SIZE;
    if (next != s_head) { s_buf[s_tail] = c; s_tail = next; }
}
static void buf_puts(const char *s) { while (*s) buf_put(*s++); }

/* Prefix state: 0=none, 'F'=F held, 'K'=K held. */
static int s_prefix;

static void translate_key(int key) {
    if (key == KEY_MK_F) { s_prefix = 'F'; return; }
    if (key == KEY_MK_K) { s_prefix = 'K'; return; }

    /* Chords (system-level, shouldn't reach here, but skip them). */
    if (KBD_IS_CHORD(key)) return;

    int pfx = s_prefix;
    s_prefix = 0;

    if (pfx == 'K') {
        /* K + key → letter */
        switch (key) {
        case KEY_MK_1:        buf_put('a'); return;
        case KEY_MK_2:        buf_put('b'); return;
        case KEY_MK_3:        buf_put('c'); return;
        case KEY_MK_4:        buf_put('d'); return;
        case KEY_MK_5:        buf_put('e'); return;
        case KEY_MK_6:        buf_put('f'); return;
        case KEY_MK_7:        buf_put('g'); return;
        case KEY_MK_8:        buf_put('h'); return;
        case KEY_MK_9:        buf_put('i'); return;
        case KEY_MK_0:        buf_put('j'); return;
        case KEY_MK_DOT:      buf_put('k'); return;
        case KEY_MK_SWAP:     buf_put('l'); return;
        case KEY_MK_ENTER:    buf_put('m'); return;
        case KEY_MK_A_UP:     buf_put('n'); return;
        case KEY_MK_EE:       buf_put('o'); return;
        case KEY_MK_NEG:      buf_put('p'); return;
        case KEY_MK_PLUS:     buf_put('q'); return;
        case KEY_MK_MINUS:    buf_put('r'); return;
        case KEY_MK_MUL:      buf_put('s'); return;
        case KEY_MK_DIV:      buf_put('t'); return;
        case KEY_MK_UPDOWN:   buf_put('u'); return;
        case KEY_MK_VO:       buf_put('v'); return;
        case KEY_MK_BP:       buf_put('w'); return;
        case KEY_MK_PP:       buf_put('x'); return;
        case KEY_MK_STEP_BCK: buf_put('y'); return;
        case KEY_MK_STEP_FWD: buf_put('z'); return;
        default:              return;
        }
    }

    if (pfx == 'F') {
        /* F + key → punctuation / multi-char operators */
        switch (key) {
        case KEY_MK_1:     buf_put('\'');   return;
        case KEY_MK_2:     buf_put(':');    return;
        case KEY_MK_3:     buf_put(',');    return;
        case KEY_MK_4:     buf_put('[');    return;
        case KEY_MK_5:     buf_put(']');    return;
        case KEY_MK_6:     buf_put('{');    return;
        case KEY_MK_7:     buf_put('}');    return;
        case KEY_MK_8:     buf_put('"');    return;
        case KEY_MK_9:     buf_put('_');    return;
        case KEY_MK_0:     buf_put('%');    return;
        case KEY_MK_DOT:   buf_put('!');    return;
        case KEY_MK_ENTER: buf_put('\t');   return;
        case KEY_MK_EE:    buf_put('#');    return;
        case KEY_MK_SWAP:  buf_puts("==");  return; /* equality */
        case KEY_MK_PLUS:  buf_puts("+=");  return;
        case KEY_MK_MINUS: buf_puts("-=");  return;
        case KEY_MK_MUL:   buf_puts("**");  return; /* power */
        case KEY_MK_DIV:   buf_puts("//");  return; /* floor div */
        case KEY_MK_NEG:   buf_put('^');    return;
        case KEY_MK_A_UP:  buf_put('&');    return;
        default:           return;
        }
    }

    /* No prefix: direct ASCII */
    switch (key) {
    case KEY_MK_0:     buf_put('0'); return;
    case KEY_MK_1:     buf_put('1'); return;
    case KEY_MK_2:     buf_put('2'); return;
    case KEY_MK_3:     buf_put('3'); return;
    case KEY_MK_4:     buf_put('4'); return;
    case KEY_MK_5:     buf_put('5'); return;
    case KEY_MK_6:     buf_put('6'); return;
    case KEY_MK_7:     buf_put('7'); return;
    case KEY_MK_8:     buf_put('8'); return;
    case KEY_MK_9:     buf_put('9'); return;
    case KEY_MK_DOT:   buf_put('.'); return;
    case KEY_MK_PLUS:  buf_put('+'); return;
    case KEY_MK_MINUS: buf_put('-'); return;
    case KEY_MK_MUL:   buf_put('*'); return;
    case KEY_MK_DIV:   buf_put('/'); return;
    case KEY_MK_NEG:   buf_put('('); return; /* /-/ → ( */
    case KEY_MK_EE:    buf_put(')'); return; /* ВП  → ) */
    case KEY_MK_SWAP:  buf_put('='); return; /* ↔   → = */
    case KEY_MK_A_UP:  buf_put(' '); return; /* А↑  → space */
    case KEY_MK_ENTER: buf_put('\n'); return;
    case KEY_ESC:      buf_put('\b'); return; /* Cx  → backspace */
    default:           return;
    }
}

int mp_hal_stdin_rx_chr(void) {
    while (s_head == s_tail) {
        int key = sys_getkey();
        translate_key(key);
    }
    char c = s_buf[s_head];
    s_head = (s_head + 1) % BUF_SIZE;
    return (unsigned char)c;
}

/* ---------- import ----------
 * No file system for Python yet: `import` finds only the built-in modules,
 * and there are no source files to lex. */

#include "py/lexer.h"
#include "py/builtin.h"
#include "py/runtime.h"
#include "py/mperrno.h"

mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    (void)filename;
    mp_raise_OSError(MP_ENOENT);
}
