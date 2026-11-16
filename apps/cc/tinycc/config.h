/* TinyCC configuration for MACJr (apps/cc): the Xtensa target only, built
 * into CC.ELF for the board, or into a cross compiler on the host for
 * testing (apps/cc/Makefile.host). Hand-written; there is no configure. */
#define TCC_VERSION "0.9.28rc-macjr"

#define TCC_TARGET_XTENSA 1

#define CONFIG_TCC_STATIC 1    /* no dlopen() */
#define CONFIG_TCC_PREDEFS 0   /* <tccdefs.h> is read from the include directory */
#define CONFIG_TCC_BCHECK 0    /* no bounds checking runtime */
#define CONFIG_TCC_BACKTRACE 0 /* no backtrace runtime */
#define CONFIG_TCC_SEMLOCK 0   /* one compile at a time */

#define CONFIG_TCCDIR "/sdcard/SRC"
#define CONFIG_TCC_SYSINCLUDEPATHS "/sdcard/SRC/INCLUDE"
#define CONFIG_TCC_LIBPATHS "/sdcard/SRC"
#define CONFIG_TCC_CRTPREFIX "/sdcard/SRC"
#define CONFIG_TCC_ELFINTERP "-"

#ifdef ESP_PLATFORM
/* CC.ELF imports the C library from the firmware; a few things it can't: */
#include <errno.h>
#include <stdio.h>
#undef errno
#define errno cc_errno /* thread-local in picolibc, which the loader can't relocate */
extern int cc_errno;
#undef stdout          /* only for -E and -v, which cc doesn't take */
#undef stderr
#define stdout ((FILE *)0)
#define stderr ((FILE *)0)
#define fflush(f) 0
#define getcwd cc_getcwd     /* no current directory on MACJr */
#define realpath cc_realpath
#endif
