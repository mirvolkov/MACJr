/*
 * UPYTHON.ELF — MicroPython interpreter for MACJr.
 *
 *   UPYTHON              REPL (the default)
 *   UPYTHON <file.py>    run the script, then exit
 *
 * The navigator starts it with the script's path when В↑ is pressed on a
 * .py file (main/main.c). A script exits with code 0, or 1 when it ended
 * with an exception (the traceback is printed first).
 *
 * In the REPL a line that opens a block (`for ...:`) continues on "..."
 * lines until an empty one; an expression's value is printed, as in any
 * Python REPL. Cx on an empty line ends a block, or exits.
 *
 * Key translation (see mphalport.c):
 *   Direct:   0-9 . + - * / = ( ) space  В↑=Enter  Cx=Backspace
 *   F+key:    ' : , [ ] { } " _ % ! # == += -= ** // ^ &  F+В↑=Tab
 *   K+key:    a-z  (1→a … 0→j, . → k, etc.)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "macjr.h"
#include "port/micropython_embed.h"
#include "py/compile.h"
#include "py/lexer.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/parse.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

#define GC_HEAP_SIZE  (256 * 1024) /* try first; fall back to 64 KB */
#define GC_HEAP_MIN   (64 * 1024)
#define REPL_LINE_MAX 256
#define REPL_BLOCK_MAX (8 * 1024)  /* one REPL statement, "..." lines included */
#define STACK_LIMIT   (12 * 1024)  /* of the 16 KB EXEC_STACK_SIZE; the rest is headroom */

static macjr_tty_t *s_tty;

void upy_set_tty(macjr_tty_t *tty); /* mphalport.c */

static void twrite(const char *s) { macjr_tty_write(s_tty, s, strlen(s)); }
static void flush(void) { macjr_display_flush(s_tty); }

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Compile and run `src`, named `name` in tracebacks. FILE input runs a
 * script; SINGLE input is one REPL statement, whose value is printed.
 * False if it ended with an exception (printed). */
static bool exec_src(const char *src, size_t len, const char *name, mp_parse_input_kind_t kind) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str(name), src, len, 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t tree = mp_parse(lex, kind);
        mp_obj_t fun = mp_compile(&tree, source_name, kind == MP_PARSE_SINGLE_INPUT);
        mp_call_function_0(fun);
        nlr_pop();
        flush();
        return true;
    }
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    flush();
    return false;
}

/* ---------- script ---------- */

/* Read the whole file into PSRAM (a large malloc goes there). open/read
 * rather than stdio: exec tracks these descriptors and closes them at a
 * kill. */
static char *read_file(const char *path, size_t *len) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t size = (size_t)st.st_size, got = 0;
    char *buf = malloc(size + 1);
    while (buf && got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) {
            free(buf);
            buf = NULL;
        } else {
            got += (size_t)n;
        }
    }
    close(fd);
    if (buf) {
        buf[size] = '\0';
        *len = size;
    }
    return buf;
}

static int run_file(const char *path) {
    const char *name = base_name(path);
    macjr_tty_header(s_tty, name);
    flush();

    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        twrite("\x1b[91mCannot read ");
        twrite(path);
        twrite("\x1b[0m\r\n");
        flush();
        return 1;
    }
    bool ok = exec_src(src, len, name, MP_PARSE_FILE_INPUT);
    free(src);
    return ok ? 0 : 1;
}

/* ---------- REPL ---------- */

/* One line with echo; В↑ ends it. False for Cx on an empty line. */
static bool read_line(char *buf, size_t size) {
    size_t len = 0;
    buf[0] = '\0';
    for (;;) {
        int c = mp_hal_stdin_rx_chr();
        if (c == '\n' || c == '\r') {
            twrite("\r\n");
            flush();
            return true;
        }
        if (c == '\b' || c == 127) {
            if (len == 0) {
                return false;
            }
            buf[--len] = '\0';
            twrite("\b \b");
            flush();
            continue;
        }
        if (c == '\t') {
            c = ' '; /* indent: four spaces per F+В↑ */
            for (int i = 0; i < 3 && len + 1 < size; i++) {
                buf[len++] = ' ';
                twrite(" ");
            }
        }
        if (c >= 0x20 && len + 1 < size) {
            buf[len++] = (char)c;
            buf[len] = '\0';
            char ch[2] = {(char)c, '\0'};
            twrite(ch);
            flush();
        }
    }
}

static void run_repl(void) {
    macjr_tty_header(s_tty, "MicroPython");
    twrite("\x1b[90mCx on an empty line: exit\x1b[0m\r\n");
    flush();

    char line[REPL_LINE_MAX];
    static char block[REPL_BLOCK_MAX]; /* not on the 16 KB stack */
    size_t block_len = 0;

    for (;;) {
        twrite(block_len ? "\x1b[90m... \x1b[0m" : "\x1b[92m>>> \x1b[0m");
        flush();

        bool got = read_line(line, sizeof(line));
        if (!got && !block_len) {
            return; /* Cx on an empty prompt */
        }
        if (got) {
            int n = snprintf(block + block_len, sizeof(block) - block_len, "%s%s", block_len ? "\n" : "", line);
            if (n > 0 && block_len + n < sizeof(block)) {
                block_len += n;
            }
            /* a block goes on until an empty line (or Cx) */
            if (mp_repl_continue_with_input(block) && line[0]) {
                continue;
            }
        }
        if (block_len) {
            exec_src(block, block_len, "<stdin>", MP_PARSE_SINGLE_INPUT);
        }
        block_len = 0;
        block[0] = '\0';
    }
}

/* ---------- entry point ---------- */

int main(int argc, char *argv[], macjr_tty_t *tty) {
    s_tty = tty;
    upy_set_tty(tty);

    size_t heap_size = GC_HEAP_SIZE;
    void *heap = malloc(heap_size);
    if (!heap) {
        heap_size = GC_HEAP_MIN;
        heap = malloc(heap_size);
    }
    if (!heap) {
        twrite("\x1b[91mNo memory for the Python heap\x1b[0m\r\n");
        flush();
        return 1;
    }

    int stack_top;
    mp_embed_init(heap, heap_size, &stack_top);
    mp_stack_set_limit(STACK_LIMIT); /* deep recursion: RuntimeError, not a crash */

    int status = 0;
    if (argc > 1) {
        status = run_file(argv[1]);
    } else {
        run_repl();
    }

    mp_embed_deinit();
    free(heap);
    return status;
}
