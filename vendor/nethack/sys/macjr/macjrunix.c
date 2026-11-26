/* NetHack 3.6	macjrunix.c	MacJr port */
/* Based on sys/unix/unixmain.c, unixunix.c and sys/share/unixtty.c. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * MACJR: the system layer for MacJr, an ESP32-S3 board. It stands in for
 * sys/unix and sys/share/unixtty.c: the game is built as UNIX (files by
 * open/read/write, the tty window port) on a system without processes,
 * users, signals, a current directory or terminal modes.
 *
 *  - one player, one game at a time; the files live in HACKDIR on the
 *    SD card, reached by full paths (NOCWD_ASSUMPTIONS: fqn_prefix[]);
 *  - the terminal is the board's screen and keys (macjrtty.c), already
 *    raw: the tty mode calls do nothing;
 *  - the screen is 40x15, so the tty port clips the map (CLIPPING) and
 *    scrolls it to follow the hero.
 *
 * The program's entry, main(argc, argv, tty), is in apps/nethack; it
 * calls nethack_main().
 */

#define NEED_VARARGS /* for error() */
#include "hack.h"
#include "dlb.h"

#include <fcntl.h>
#include <sys/stat.h>

extern int64_t esp_timer_get_time(void); /* esp_timer.h */

int macjr_errno; /* errno, see macjrio.h */

static void FDECL(process_options, (int, char **));
static void NDECL(set_prefixes);

/* the playground, with a slash: every file's prefix */
#define PLAYGROUND HACKDIR "/"

int
nethack_main(argc, argv)
int argc;
char *argv[];
{
    int fd;
    boolean resuming = FALSE; /* assume new game */
    boolean plsel_once = FALSE;

    sys_early_init();
    ru_init(); /* keep the English names for the parsers */
    hname = "nethack";
    hackpid = 1;
    set_prefixes();
    (void) mkdir(PLAYGROUND "save", 0777); /* if not there yet */

    choose_windows(DEFAULT_WINDOW_SYS);
    initoptions();

    u.uhp = 1; /* prevent RIP on early quits */
    program_state.preserve_locks = 1;

    process_options(argc, argv); /* command line options */
    init_nhwindows(&argc, argv); /* now we can set up window system */

    set_playmode(); /* sets plname to "wizard" for wizard mode */
    /* strip role,race,&c suffix; calls askname() if plname[] is empty */
    plnamesuffix();
    locknum = 0; /* lock and level files by the hero's name */

    dlb_init(); /* must be before newgame() */

    /*
     * Initialize the vision system.  This must be before mklev() on a
     * new game or before a level restore on a saved game.
     */
    vision_init();

    display_gamewindows();

attempt_restore:
    if (*plname) {
        getlock();
        program_state.preserve_locks = 0; /* after getlock() */
    }

    if (*plname && (fd = restore_saved_game()) >= 0) {
        pline("Восстановление сохранения...");
        mark_synch(); /* flush output */
        if (dorecover(fd)) {
            resuming = TRUE; /* not starting new game */
            wd_message();
            if (discover || wizard) {
                if (yn("Оставить файл сохранения?") == 'n')
                    (void) delete_savefile();
            }
        }
    }

    if (!resuming) {
        boolean neednewlock = (!*plname);

        /* new game:  start by choosing role, race, etc;
           player might change the hero's name while doing that,
           in which case we try to restore under the new name
           and skip selection this time if that didn't succeed */
        if (!iflags.renameinprogress || iflags.defer_plname || neednewlock) {
            if (!plsel_once)
                player_selection();
            plsel_once = TRUE;
            if (neednewlock && *plname)
                goto attempt_restore;
            if (iflags.renameinprogress) {
                delete_levelfile(0); /* remove empty lock file */
                getlock();
                goto attempt_restore;
            }
        }
        newgame();
        wd_message();
    }

    /* moveloop() never returns but isn't flagged NORETURN */
    moveloop(resuming);

    exit(EXIT_SUCCESS);
    /*NOTREACHED*/
    return 0;
}

/* Every kind of file in the playground on the card. */
static void
set_prefixes()
{
    int i;

    for (i = 0; i < PREFIX_COUNT; i++)
        fqn_prefix[i] = PLAYGROUND;
}

/* caveat: argv elements might be arbitrary long */
static void
process_options(argc, argv)
int argc;
char *argv[];
{
    int i;

    while (argc > 1 && argv[1][0] == '-') {
        argv++;
        argc--;
        switch (argv[0][1]) {
        case 'D':
            wizard = TRUE, discover = FALSE;
            break;
        case 'X':
            discover = TRUE, wizard = FALSE;
            break;
        case 'u':
            if (argv[0][2]) {
                (void) strncpy(plname, argv[0] + 2, sizeof plname - 1);
            } else if (argc > 1) {
                argc--;
                argv++;
                (void) strncpy(plname, argv[0], sizeof plname - 1);
            }
            break;
        case 'p': /* profession (role) */
            if (argv[0][2]) {
                if ((i = str2role(&argv[0][2])) >= 0)
                    flags.initrole = i;
            } else if (argc > 1) {
                argc--;
                argv++;
                if ((i = str2role(argv[0])) >= 0)
                    flags.initrole = i;
            }
            break;
        case 'r': /* race */
            if (argv[0][2]) {
                if ((i = str2race(&argv[0][2])) >= 0)
                    flags.initrace = i;
            } else if (argc > 1) {
                argc--;
                argv++;
                if ((i = str2race(argv[0])) >= 0)
                    flags.initrace = i;
            }
            break;
        case '@':
            flags.randomall = 1;
            break;
        default:
            if ((i = str2role(&argv[0][1])) >= 0)
                flags.initrole = i;
            break;
        }
    }
}

/*
 * The level files of the game in progress are <name>.0, .1, ...; <name>.0
 * is the lock. One left from a game that didn't end (the program was
 * killed, the board lost power) can be thrown away and the game started
 * again; recovering it needs the recover utility (not on the board).
 */
void
getlock()
{
    struct stat st;
    const char *fq_lock;
    int fd, i;

    Strcpy(lock, plname);
    regularize(lock);
    set_levelfile_name(lock, 0);
    fq_lock = fqname(lock, LEVELPREFIX, 0);

    if (stat(fq_lock, &st) == 0) {
        if (yn("Под вашим именем уже идёт игра. Удалить её?")
            != 'y')
            error("%s", "");
        for (i = 0; i <= MAXDUNGEON * MAXLEVEL + 1; i++)
            delete_levelfile(i);
    }

    fd = creat(fq_lock, FCMASK);
    if (fd == -1)
        error("cannot creat lock file (%s).", fq_lock);
    if (write(fd, (genericptr_t) &hackpid, sizeof hackpid)
        != sizeof hackpid)
        error("cannot write lock (%s)", fq_lock);
    if (close(fd) == -1)
        error("cannot close lock (%s)", fq_lock);
}

/* normalize file name - we don't like .'s, /'s, spaces */
void
regularize(s)
register char *s;
{
    register char *lp;

    while ((lp = index(s, '.')) != 0 || (lp = index(s, '/')) != 0
           || (lp = index(s, ' ')) != 0)
        *lp = '_';
}

/* ---- the terminal: the board's tty is raw already ---- */

/* line editing keys of getlin() (unixtty.c gets them from the tty):
   Backspace, ^U */
char erase_char = '\b', intr_char = '\003', kill_char = '\025';

void
gettty()
{
}

void
settty(s)
const char *s;
{
    end_screen();
    if (s)
        raw_print(s);
}

void
setftty()
{
    start_screen();
}

void
intron()
{
}

void
introff()
{
}

/* the screen size, for termcap.c */
void
get_scr_size()
{
    macjr_winsize(&CO, &LI);
}

/* fatal error: show it until a key, then end the program */
/*VARARGS1*/
void error
VA_DECL(const char *, s)
{
    VA_START(s);
    VA_INIT(s, const char *);

    if (iflags.window_inited)
        exit_nhwindows((char *) 0);
    Vprintf(s, VA_ARGS);
    (void) puts("");
    VA_END();
    if (*s) {
        (void) fputs("(любая клавиша)", stdout);
        (void) macjr_getch();
    }
    exit(EXIT_FAILURE);
}

/* ---- what unixmain.c gives the rest of the game ---- */

void
sethanguphandler(handler)
void FDECL((*handler), (int));
{
    nhUse(handler);
}

/* wizard mode by -D, for anyone: the board has one user */
boolean
authorize_wizard_mode()
{
    return TRUE;
}

void
wd_message()
{
    if (wizard)
        You("are in debug mode.");
    else if (discover)
        You("are in non-scoring explore/discovery mode.");
}

boolean
check_user_string(optstr)
char *optstr;
{
    nhUse(optstr);
    return TRUE;
}

char *
get_login_name()
{
    static char name[] = "player";

    return name;
}

unsigned long
sys_random_seed()
{
    /* time of day and microseconds since boot */
    return (unsigned long) getnow() ^ (unsigned long) esp_timer_get_time();
}

/* add a trailing slash to a directory name, as fqn_prefix[] wants it */
void
append_slash(name)
char *name;
{
    char *ptr;

    if (!*name)
        return;
    ptr = name + (strlen(name) - 1);
    if (*ptr != '/') {
        *++ptr = '/';
        *++ptr = '\0';
    }
}

void
after_opt_showpaths(dir)
const char *dir UNUSED;
{
    nh_terminate(EXIT_SUCCESS);
}

/* ---- the C library calls a program on the board doesn't get ---- */

/* the environment: HOME is the playground, for .nethackrc */
char *
getenv(name)
const char *name;
{
    static char home[] = HACKDIR;

    if (!strcmp(name, "HOME"))
        return home;
    return (char *) 0;
}

uid_t
getuid()
{
    return 0;
}

pid_t
getpid()
{
    return 1;
}

int
creat(path, mode)
const char *path;
mode_t mode;
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int
chmod(path, mode)
const char *path;
mode_t mode;
{
    nhUse(path);
    nhUse(mode);
    return 0;
}

mode_t
umask(mask)
mode_t mask;
{
    nhUse(mask);
    return 0;
}

/*macjrunix.c*/
