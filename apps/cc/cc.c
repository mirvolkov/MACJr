/*
 * CC.ELF -- the C compiler: TinyCC (vendor/tinycc) with an Xtensa code
 * generator, writing programs that the MACJr loader runs.
 *
 *   cc [-o out.elf] [-Dname[=value]] [-Idir] file.c [file.c ...]
 *
 * The navigator opens a .c file with it. Without -o the program is written
 * next to the first source, with .elf in place of .c. Headers come from
 * SRC/INCLUDE on the card.
 *
 *   ════════════════[C compiler]════════════
 *   hello.c
 *   -> /sdcard/SRC/hello.elf, 412 bytes
 *
 *    Cx exit
 *
 * TinyCC compiles everything into one relocatable object in memory; this
 * file then writes the shared-object ELF the loader (espressif/elf_loader,
 * section mode) accepts: .text, .rodata, .data and .bss, with .rela.dyn
 * holding R_XTENSA_RELATIVE for the program's own addresses and
 * R_XTENSA_GLOB_DAT for firmware symbols, which the loader looks up by
 * name in .dynsym. The entry point is main().
 *
 * Built twice: into CC.ELF by apps/Makefile, and as a cross compiler on the
 * host (apps/cc/Makefile.host) to check the generated code there.
 */
#define ONE_SOURCE 1
#include "libtcc.c" /* vendor/tinycc */

#ifdef ESP_PLATFORM
#include <pthread.h>
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#endif

#ifdef ESP_PLATFORM
/* what config.h redirects on the board */
int cc_errno;

char *cc_getcwd(char *buf, size_t size)
{
    if (!buf || size < 2)
        return NULL;
    strcpy(buf, "/");
    return buf;
}

/* TinyCC only calls realpath(path, NULL) and frees the result with
   libc_free() */
#undef malloc
char *cc_realpath(const char *path, char *resolved)
{
    char *r = resolved ? resolved : malloc(strlen(path) + 1);
    if (r)
        strcpy(r, path);
    return r;
}
#define malloc(s) use_tcc_malloc(s)
#endif

#ifndef CC_SYSINCLUDE
#define CC_SYSINCLUDE "/sdcard/SRC/INCLUDE"
#endif

#define MAX_INPUTS 16
#define TEXT_VADDR 0x100 /* nothing at 0, which reads as NULL */

/* ------------------------------------------------------------------------- */
/* messages: the tty on the board, stderr on the host */

#ifdef ESP_PLATFORM
static macjr_tty_t *s_tty;

static void say(const char *color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const char *color, const char *fmt, ...)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s", color);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    macjr_tty_write(s_tty, buf, strlen(buf));
    macjr_tty_write(s_tty, "\x1b[0m\r\n", 6);
    macjr_display_flush(s_tty);
}
#else
static void say(const char *color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const char *color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#endif

#define C_ERR  "\x1b[91m"
#define C_WARN "\x1b[93m"
#define C_OK   "\x1b[92m"
#define C_INFO "\x1b[97m"
#define C_DIM  "\x1b[90m"

static int s_errors;

/* TinyCC's errors and warnings: "path:line: error: ..." -> "file:line: ..." */
static void tcc_message(void *opaque, const char *msg)
{
    const char *slash = strrchr(msg, '/');
    const char *colon = strchr(msg, ':');
    int is_warning = strstr(msg, "warning:") != NULL;
    if (slash && colon && slash < colon)
        msg = slash + 1;
    if (!is_warning)
        s_errors++;
    say(is_warning ? C_WARN : C_ERR, "%s", msg);
}

/* ------------------------------------------------------------------------- */
/* the program ELF */

enum { O_TEXT, O_RODATA, O_DATA, O_BSS, O_N };

static const char *const out_name[O_N] = {".text", ".rodata", ".data", ".bss"};

typedef struct {
    unsigned char *data;  /* NULL for .bss */
    uint32_t vaddr, size, align;
} out_sec_t;

typedef struct {
    unsigned char *buf;
    size_t len, cap;
} buf_t;

static void *buf_add(buf_t *b, const void *p, size_t n)
{
    void *at;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n)
            cap *= 2;
        b->buf = tcc_realloc(b->buf, cap);
        b->cap = cap;
    }
    at = b->buf + b->len;
    if (p)
        memcpy(at, p, n);
    else
        memset(at, 0, n);
    b->len += n;
    return at;
}

static void buf_align(buf_t *b, size_t a)
{
    while (b->len & (a - 1))
        buf_add(b, NULL, 1);
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static int out_kind(Section *s)
{
    if (s->sh_flags & SHF_EXECINSTR)
        return O_TEXT;
    if (s->sh_type == SHT_NOBITS)
        return O_BSS;
    if (s->sh_flags & SHF_WRITE)
        return O_DATA;
    return O_RODATA;
}

typedef struct {
    int kind;      /* -1: not in the program */
    uint32_t vaddr;
} sec_place_t;

typedef struct {
    buf_t syms, strs, relas;
    int nsyms;
} dyn_t;

/* .dynsym index of firmware symbol `name`, added on first use */
static int dyn_sym(dyn_t *d, const char *name)
{
    ElfW(Sym) *syms = (ElfW(Sym) *)d->syms.buf, sym;
    int i;
    for (i = 1; i < d->nsyms; i++)
        if (!strcmp((char *)d->strs.buf + syms[i].st_name, name))
            return i;
    memset(&sym, 0, sizeof(sym));
    sym.st_name = d->strs.len;
    sym.st_info = ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE);
    sym.st_shndx = SHN_UNDEF;
    buf_add(&d->strs, name, strlen(name) + 1);
    buf_add(&d->syms, &sym, sizeof(sym));
    return d->nsyms++;
}

static void dyn_rela(dyn_t *d, uint32_t at, int sym, int type)
{
    ElfW_Rel r;
    r.r_offset = at;
    r.r_info = ELFW(R_INFO)(sym, type);
    r.r_addend = 0;
    buf_add(&d->relas, &r, sizeof(r));
}

static int write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0)
            return -1;
        c += w;
        n -= w;
    }
    return 0;
}

#ifdef ESP_PLATFORM
extern uintptr_t elf_find_sym(const char *sym_name);
#endif

/* Write the program ELF for the object TinyCC compiled. Returns its size,
   or -1 after reporting why not. */
static long write_program(TCCState *s1, const char *path)
{
    out_sec_t out[O_N];
    sec_place_t *place;
    dyn_t dyn;
    ElfW(Sym) *symtab = (ElfW(Sym) *)symtab_section->data;
    const char *strtab = (const char *)symtab_section->link->data;
    int nsyms = symtab_section->data_offset / sizeof(ElfW(Sym));
    int i, k, main_idx, missing = 0, ret = -1;
    uint32_t vaddr, entry;

    memset(out, 0, sizeof(out));
    memset(&dyn, 0, sizeof(dyn));
    place = tcc_mallocz(s1->nb_sections * sizeof(*place));

    /* place the sections */
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        place[i].kind = -1;
        if (!(s->sh_flags & SHF_ALLOC) || !s->data_offset)
            continue;
        if (strstr(s->name, "init_array") || strstr(s->name, "fini_array")) {
            say(C_ERR, "constructors and destructors are not supported");
            goto done;
        }
        k = out_kind(s);
        place[i].kind = k;
        if (s->sh_addralign > out[k].align)
            out[k].align = s->sh_addralign;
        out[k].size = align_up(out[k].size, s->sh_addralign ? s->sh_addralign : 1);
        place[i].vaddr = out[k].size; /* offset in its output section, for now */
        out[k].size += s->data_offset;
    }
    if (!out[O_TEXT].size) {
        say(C_ERR, "no code");
        goto done;
    }
    vaddr = TEXT_VADDR;
    for (k = 0; k < O_N; k++) {
        if (out[k].align < 4)
            out[k].align = 4;
        vaddr = align_up(vaddr, 16);
        out[k].vaddr = vaddr;
        vaddr += out[k].size;
        if (k != O_BSS && out[k].size)
            out[k].data = tcc_mallocz(out[k].size);
    }
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        if (place[i].kind < 0)
            continue;
        k = place[i].kind;
        if (out[k].data)
            memcpy(out[k].data + place[i].vaddr, s->data, s->data_offset);
        place[i].vaddr += out[k].vaddr;
    }

    /* relocations */
    buf_add(&dyn.syms, NULL, sizeof(ElfW(Sym)));
    buf_add(&dyn.strs, NULL, 1);
    dyn.nsyms = 1;
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i], *sr = s->reloc;
        ElfW_Rel *rel;
        if (place[i].kind < 0 || !sr)
            continue;
        for_each_elem(sr, 0, rel, ElfW_Rel) {
            int type = ELFW(R_TYPE)(rel->r_info);
            ElfW(Sym) *sym = &symtab[ELFW(R_SYM)(rel->r_info)];
            const char *name = strtab + sym->st_name;
            uint32_t at = place[i].vaddr + rel->r_offset;
            unsigned char *ptr = out[place[i].kind].data + (at - out[place[i].kind].vaddr);
            if (type != R_XTENSA_32) {
                say(C_ERR, "unsupported relocation %d", type);
                goto done;
            }
            if (sym->st_shndx == SHN_UNDEF) {
                if (ELFW(ST_BIND)(sym->st_info) == STB_WEAK) {
                    write32le(ptr, rel->r_addend);
                    continue;
                }
                if (rel->r_addend) {
                    say(C_ERR, "%s+%d: no offsets to firmware symbols", name, (int)rel->r_addend);
                    goto done;
                }
#ifdef ESP_PLATFORM
                if (!elf_find_sym(name)) {
                    say(C_ERR, "%s: not in the firmware", name);
                    missing++;
                }
#endif
                write32le(ptr, 0);
                dyn_rela(&dyn, at, dyn_sym(&dyn, name), R_XTENSA_GLOB_DAT);
            } else if (sym->st_shndx == SHN_ABS) {
                write32le(ptr, sym->st_value + rel->r_addend);
            } else if (sym->st_shndx < s1->nb_sections && place[sym->st_shndx].kind >= 0) {
                write32le(ptr, place[sym->st_shndx].vaddr + sym->st_value + rel->r_addend);
                dyn_rela(&dyn, at, 0, R_XTENSA_RELATIVE);
            } else {
                say(C_ERR, "%s: symbol in no section", *name ? name : "?");
                goto done;
            }
        }
    }
    if (missing)
        goto done;

    /* entry point */
    main_idx = find_elf_sym(symtab_section, "main");
    if (!main_idx || symtab[main_idx].st_shndx == SHN_UNDEF
        || place[symtab[main_idx].st_shndx].kind != O_TEXT) {
        say(C_ERR, "no main()");
        goto done;
    }
    entry = place[symtab[main_idx].st_shndx].vaddr + symtab[main_idx].st_value;

    {
        /* file: header, section contents, tables, section headers */
        enum { S_NULL, S_TEXT, S_RODATA, S_DATA, S_BSS, S_DYNSYM, S_DYNSTR, S_RELA, S_SHSTR, S_N };
        static const char shstr[] = "\0.text\0.rodata\0.data\0.bss\0.dynsym\0.dynstr\0.rela.dyn\0.shstrtab";
        static const int shname[S_N] = {0, 1, 7, 15, 21, 26, 34, 42, 52};
        ElfW(Shdr) sh[S_N];
        ElfW(Ehdr) eh;
        int map[S_N], nsh = 0, fd;
        buf_t f;

        memset(&f, 0, sizeof(f));
        memset(sh, 0, sizeof(sh));
        buf_add(&f, NULL, sizeof(eh));
        map[S_NULL] = nsh++;
        for (k = 0; k < O_N; k++) {
            ElfW(Shdr) *h;
            if (!out[k].size && k != O_TEXT) {
                map[S_TEXT + k] = -1;
                continue;
            }
            map[S_TEXT + k] = nsh;
            h = &sh[nsh++];
            buf_align(&f, 4);
            h->sh_name = shname[S_TEXT + k];
            h->sh_type = k == O_BSS ? SHT_NOBITS : SHT_PROGBITS;
            h->sh_flags = SHF_ALLOC | (k == O_TEXT ? SHF_EXECINSTR : 0)
                | (k == O_DATA || k == O_BSS ? SHF_WRITE : 0);
            h->sh_addr = out[k].vaddr;
            h->sh_offset = f.len;
            h->sh_size = out[k].size;
            h->sh_addralign = out[k].align;
            if (out[k].data)
                buf_add(&f, out[k].data, out[k].size);
        }
        map[S_DYNSYM] = nsh;
        buf_align(&f, 4);
        sh[nsh].sh_name = shname[S_DYNSYM];
        sh[nsh].sh_type = SHT_DYNSYM;
        sh[nsh].sh_flags = SHF_ALLOC;
        sh[nsh].sh_offset = f.len;
        sh[nsh].sh_size = dyn.syms.len;
        sh[nsh].sh_link = nsh + 1;
        sh[nsh].sh_info = 1;
        sh[nsh].sh_addralign = 4;
        sh[nsh].sh_entsize = sizeof(ElfW(Sym));
        buf_add(&f, dyn.syms.buf, dyn.syms.len);
        nsh++;
        map[S_DYNSTR] = nsh;
        sh[nsh].sh_name = shname[S_DYNSTR];
        sh[nsh].sh_type = SHT_STRTAB;
        sh[nsh].sh_flags = SHF_ALLOC;
        sh[nsh].sh_offset = f.len;
        sh[nsh].sh_size = dyn.strs.len;
        sh[nsh].sh_addralign = 1;
        buf_add(&f, dyn.strs.buf, dyn.strs.len);
        nsh++;
        map[S_RELA] = nsh;
        buf_align(&f, 4);
        sh[nsh].sh_name = shname[S_RELA];
        sh[nsh].sh_type = SHT_RELA;
        sh[nsh].sh_flags = SHF_ALLOC;
        sh[nsh].sh_offset = f.len;
        sh[nsh].sh_size = dyn.relas.len;
        sh[nsh].sh_link = map[S_DYNSYM];
        sh[nsh].sh_addralign = 4;
        sh[nsh].sh_entsize = sizeof(ElfW_Rel);
        buf_add(&f, dyn.relas.buf, dyn.relas.len);
        nsh++;
        map[S_SHSTR] = nsh;
        sh[nsh].sh_name = shname[S_SHSTR];
        sh[nsh].sh_type = SHT_STRTAB;
        sh[nsh].sh_offset = f.len;
        sh[nsh].sh_size = sizeof(shstr);
        sh[nsh].sh_addralign = 1;
        buf_add(&f, shstr, sizeof(shstr));
        nsh++;
        /* .bss takes no file space; give it an offset inside the file */
        if (map[S_BSS] >= 0)
            sh[map[S_BSS]].sh_offset = sh[map[S_SHSTR]].sh_offset;

        buf_align(&f, 4);
        memset(&eh, 0, sizeof(eh));
        memcpy(eh.e_ident, ELFMAG, SELFMAG);
        eh.e_ident[EI_CLASS] = ELFCLASS32;
        eh.e_ident[EI_DATA] = ELFDATA2LSB;
        eh.e_ident[EI_VERSION] = EV_CURRENT;
        eh.e_type = ET_DYN;
        eh.e_machine = EM_XTENSA;
        eh.e_version = EV_CURRENT;
        eh.e_entry = entry;
        eh.e_shoff = f.len;
        eh.e_flags = 0x300; /* as GCC sets it for the ESP32-S3 */
        eh.e_ehsize = sizeof(eh);
        eh.e_shentsize = sizeof(ElfW(Shdr));
        eh.e_shnum = nsh;
        eh.e_shstrndx = map[S_SHSTR];
        memcpy(f.buf, &eh, sizeof(eh));
        buf_add(&f, sh, nsh * sizeof(ElfW(Shdr)));

        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
        if (fd < 0) {
            say(C_ERR, "can't create %s", path);
        } else {
            if (write_all(fd, f.buf, f.len) == 0)
                ret = f.len;
            else
                say(C_ERR, "can't write %s", path);
            close(fd);
        }
        tcc_free(f.buf);
    }

done:
    for (k = 0; k < O_N; k++)
        tcc_free(out[k].data);
    tcc_free(dyn.syms.buf);
    tcc_free(dyn.strs.buf);
    tcc_free(dyn.relas.buf);
    tcc_free(place);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* the driver */

typedef struct {
    int argc;
    char **argv;
    const char *inputs[MAX_INPUTS];
    int ninputs;
    char out[256];
    long size; /* of the program written, -1 if none */
    int lines;          /* compiled, headers included */
    unsigned bytes;
    unsigned compile_ms, write_ms;
} job_t;

/* milliseconds, for the statistics */
#ifdef ESP_PLATFORM
static unsigned now_ms(void)
{
    return sys_millis();
}
#else
#include <sys/time.h>
static unsigned now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000u + tv.tv_usec / 1000;
}
#endif

static int parse_args(job_t *job)
{
    int i;
    job->ninputs = 0;
    job->out[0] = 0;
    for (i = 1; i < job->argc; i++) {
        const char *a = job->argv[i];
        if (!strcmp(a, "-o") && i + 1 < job->argc) {
            pstrcpy(job->out, sizeof(job->out), job->argv[++i]);
        } else if (a[0] == '-') {
            continue; /* passed to TinyCC in compile() */
        } else if (job->ninputs < MAX_INPUTS) {
            job->inputs[job->ninputs++] = a;
        } else {
            say(C_ERR, "too many files");
            return -1;
        }
    }
    if (!job->ninputs) {
        say(C_INFO, "usage: cc [-o out.elf] file.c...");
        return -1;
    }
    if (!job->out[0]) {
        /* hello.c -> hello.elf, HELLO.C -> HELLO.ELF */
        const char *in = job->inputs[0];
        const char *dot = strrchr(in, '.'), *slash = strrchr(in, '/');
        size_t base = dot && (!slash || dot > slash) ? (size_t)(dot - in) : strlen(in);
        int upper = dot && dot[1] >= 'A' && dot[1] <= 'Z';
        if (base + 5 > sizeof(job->out))
            base = sizeof(job->out) - 5;
        memcpy(job->out, in, base);
        strcpy(job->out + base, upper ? ".ELF" : ".elf");
    }
    return 0;
}

static void compile(job_t *job)
{
    TCCState *s;
    int i;
    unsigned t0 = now_ms(), t1;

    job->size = -1;
    job->lines = 0;
    s_errors = 0;
    s = tcc_new();
    if (!s) {
        say(C_ERR, "out of memory");
        return;
    }
    tcc_set_error_func(s, NULL, tcc_message);
    s->nostdinc = 1;
    s->nostdlib = 1;
    tcc_add_sysinclude_path(s, CC_SYSINCLUDE);
    for (i = 1; i < job->argc; i++) {
        const char *a = job->argv[i];
        if (!strcmp(a, "-o"))
            i++;
        else if (a[0] == '-')
            tcc_set_options(s, a);
    }
    tcc_set_output_type(s, TCC_OUTPUT_OBJ);
    s->filetype = AFF_TYPE_C; /* whatever the extension's case */
    for (i = 0; i < job->ninputs; i++) {
        const char *slash = strrchr(job->inputs[i], '/');
        say(C_INFO, "%s", slash ? slash + 1 : job->inputs[i]);
        if (tcc_add_file(s, job->inputs[i]) < 0 || s_errors)
            goto done;
    }
    t1 = now_ms();
    job->compile_ms = t1 - t0;
    {
        TCCState *s1 = s; /* total_lines and total_bytes are TCC_STATE_VARs */
        job->lines = total_lines;
        job->bytes = total_bytes;
    }
    job->size = write_program(s, job->out);
    job->write_ms = now_ms() - t1;
done:
    tcc_delete(s);
}

/* what was written, and how fast */
static void report(job_t *job)
{
    unsigned ms = job->compile_ms ? job->compile_ms : 1;
    say(C_OK, "-> %s", job->out);
    say(C_INFO, "%ld bytes", job->size);
    say(C_INFO, "%d lines, %u KB with headers", job->lines, (job->bytes + 512) / 1024);
    say(C_INFO, "compile %u ms, write %u ms", job->compile_ms, job->write_ms);
    say(C_INFO, "%u lines/s", (unsigned)(job->lines * 1000ull / ms));
}

#ifdef ESP_PLATFORM

/* TinyCC's memory in PSRAM: it makes many small blocks, which would
   otherwise take the internal RAM the firmware needs. */
static void *psram_realloc(void *ptr, unsigned long size)
{
    if (!size) {
        heap_caps_free(ptr);
        return NULL;
    }
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* TinyCC recurses deeply: it runs in a thread with a big stack in PSRAM,
   on the program's core and priority (CPU1, idle priority), so a long
   compile doesn't starve the firmware or trip the task watchdog. */
#define CC_STACK (96 * 1024)

static void *compile_thread(void *arg)
{
    compile(arg);
    return NULL;
}

static void wait_exit(void)
{
    int fd = open(KBD_DEV_PATH, O_RDONLY);
    uint8_t key;
    say(C_DIM, "\r\n Cx exit");
    while (fd >= 0 && read(fd, &key, 1) == 1) {
        if (key == KEY_ESC && !monitor_is_open())
            break;
    }
    if (fd >= 0)
        close(fd);
}

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    static job_t job;
    esp_pthread_cfg_t cfg;
    pthread_t th;

    s_tty = tty;
    macjr_tty_write(tty, "\x1b[?25l\x1b[0m\x1b[2J", 14);
    macjr_tty_header(tty, "C compiler");
    macjr_tty_write(tty, "\x1b[2;1H", 6);
    macjr_display_flush(tty);

    job.argc = argc;
    job.argv = argv;
    if (parse_args(&job) == 0) {
        tcc_set_realloc(psram_realloc);
        cfg = esp_pthread_get_default_config();
        cfg.stack_size = CC_STACK;
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        cfg.prio = 0;
        cfg.pin_to_core = 1;
        cfg.thread_name = "cc";
        esp_pthread_set_cfg(&cfg);
        if (pthread_create(&th, NULL, compile_thread, &job) == 0)
            pthread_join(th, NULL);
        else
            say(C_ERR, "no memory for the compiler's stack");
        if (job.size >= 0)
            report(&job);
    }
    wait_exit();
    return job.size >= 0 ? 0 : 1;
}

#else /* host */

int main(int argc, char *argv[])
{
    job_t job;
    job.argc = argc;
    job.argv = argv;
    if (parse_args(&job))
        return 2;
    compile(&job);
    if (job.size < 0)
        return 1;
    report(&job);
    return 0;
}

#endif
