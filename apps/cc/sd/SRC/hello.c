/* hello.c -- a first program to build on the board: open it in the
 * navigator, and CC.ELF compiles it into hello.elf next to it. */
#include <stdio.h>
#include <string.h>
#include <macjr.h>

static void print(macjr_tty_t *tty, const char *s)
{
    macjr_tty_write(tty, s, strlen(s));
}

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    char buf[40];
    int i;

    print(tty, "\x1b[0m\x1b[2J");
    macjr_tty_header(tty, "Hello");
    print(tty, "\x1b[3;1H\x1b[97mHello from a program\r\ncompiled on the board!\r\n\r\n");
    for (i = 1; i <= 5; i++) {
        snprintf(buf, sizeof(buf), "\x1b[96m%d x %d = %d\r\n", i, i, i * i);
        print(tty, buf);
    }
    snprintf(buf, sizeof(buf), "\r\n\x1b[90mup for %u ms", sys_millis());
    print(tty, buf);
    print(tty, "\x1b[15;1H\x1b[90m any key: exit");
    macjr_display_flush(tty);
    sys_getkey();
    return 0;
}
