/* lines.c -- printf() on the screen: a status panel redrawn in place,
 * a row at a time, with a line() helper of its own taking "...". Keys:
 * + and - change the value, Cx exits. */
#include <stdarg.h>
#include <stdio.h>
#include <macjr.h>

#define ROW_TITLE 3
#define ROW_VALUE 5
#define ROW_STATE 6
#define ROW_HINT  15

/* Move to row r (1-based), print the formatted text, clear the rest. */
static void line(int r, const char *fmt, ...)
{
    va_list ap;
    printf("\x1b[%d;1H", r);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\x1b[0m\x1b[K");
}

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    int value = 7, key;

    printf("\x1b[?25l\x1b[0m\x1b[2J");
    macjr_tty_header(tty, "printf");
    line(ROW_TITLE, "\x1b[97mprintf() goes to /dev/tty");
    line(ROW_HINT, "\x1b[90m + - change   Cx exit");
    for (;;) {
        line(ROW_VALUE, "\x1b[96mValue     \x1b[97m%3d", value);
        if (value > 0)
            line(ROW_STATE, "\x1b[96mOutputs   \x1b[92mON");
        else
            line(ROW_STATE, "\x1b[96mOutputs   \x1b[91mOFF");
        key = sys_getkey();
        if (key == KEY_ESC)
            break;
        if (key == KEY_PLUS && value < 999)
            value++;
        if (key == KEY_MINUS && value > -99)
            value--;
    }
    printf("\x1b[?25h");
    return 0;
}
