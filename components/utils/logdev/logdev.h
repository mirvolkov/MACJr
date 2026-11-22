#ifndef MACJR_LOGDEV_H
#define MACJR_LOGDEV_H

#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "esp_err.h"
#include "sdcard.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The system log: the character device LOG_DEV, a journal of records any
 * program can write and read, as Linux's /dev/kmsg.
 *
 * A record is a block: a number, a level, the program that wrote it, a tag
 * of the writer's choice and a line of text (log_rec_t). There is no
 * time: the number, incremented with every record, gives the order. The
 * firmware fills in the number and the program; nobody writes a record in
 * another program's name.
 *
 * Writing: one write() is one record. The text may start with "<L>tag: "
 * to give the level (a letter of LOG_LEVELS) and the tag: "<W>net: lost
 * link". Without it the level is LOG_INFO and the tag empty. Text past
 * LOG_BODY_MAX is cut, a final newline dropped. log_write() does all that
 * with printf formatting.
 *
 * Reading: read() gives one whole record per call, the next one after the
 * last read, a log_rec_t of LOG_REC_SIZE(rec) bytes (the body with its
 * terminator). It blocks until there is one, or fails with EAGAIN under
 * O_NONBLOCK; select() reports the descriptor readable while records are
 * waiting. A new descriptor starts at the oldest record kept;
 * lseek(fd, 0, SEEK_END) skips to new ones only, SEEK_SET back to the
 * oldest. A reader that falls behind by more than LOG_RING records goes on
 * from the oldest still kept, and that record has LOG_LOST set.
 *
 * Keeping: the last LOG_RING records stay in memory (PSRAM). Every
 * LOG_FLUSH_MS the new ones are appended to text files on the SD card, in
 * LOG_DIR, one per program named after it, a line per record:
 *
 *   LOG/SI5351_CONTROL.LOG
 *     1042 I si5351 CLK0 7.055000 MHz
 *     1043 E si5351 setting 7055000 Hz failed: ESP_ERR_TIMEOUT
 *
 * (number, level letter, tag or "-", text; control characters in the
 * text become spaces). A file that would grow past LOG_FILE_MAX becomes
 * <name>.OLD, replacing the one before, and a new <name>.LOG starts. At
 * the next boot the newest records of all the files come back into
 * memory, and the numbers go on from there. While the card is lent out
 * (USB drive, log_suspend) records wait in memory.
 */
#define LOG_DEV "/dev/log"
#define LOG_DIR SDCARD_MOUNT_POINT "/LOG"

#define LOG_RING          512  /* records in memory */
#define LOG_BODY_MAX      160  /* text bytes, terminator included */
#define LOG_NAME_MAX      16   /* app and tag, terminator included */
#define LOG_MAX_FDS       8    /* descriptors open at the same time */
#define LOG_FLUSH_MS      1000
#define LOG_FILE_MAX      32768 /* bytes of a .LOG file; the .OLD as much again */
#define LOG_MAX_FILES     32    /* programs with a file in LOG_DIR, read at boot */

/* Levels, most severe first, and their letters for "<L>" */
enum { LOG_ERROR = 0, LOG_WARN, LOG_INFO, LOG_DEBUG };
#define LOG_LEVELS "EWID"

/* flags */
#define LOG_LOST 0x01 /* records before this one were lost to this reader */

typedef struct {
  uint32_t id;    /* 1, 2, ...; never reused, goes on across boots */
  uint8_t level;  /* LOG_ERROR .. LOG_DEBUG */
  uint8_t flags;  /* LOG_LOST */
  uint16_t len;   /* body length, terminator not counted */
  char app[LOG_NAME_MAX];  /* the program, or the firmware task */
  char tag[LOG_NAME_MAX];
  char body[LOG_BODY_MAX]; /* terminated */
} log_rec_t;

/* Bytes of a record as read() gives it, and the fixed part before body */
#define LOG_REC_HEAD offsetof(log_rec_t, body)
#define LOG_REC_SIZE(rec) (LOG_REC_HEAD + (rec)->len + 1)

/* Start the log and register LOG_DEV. Call once, after the SD card is
 * mounted (or without one): the records saved on it come back then. */
esp_err_t log_init(void);

/* The card goes away (lent to a USB host): save what is waiting, then
 * keep records in memory only. log_resume() when it is back. */
void log_suspend(void);
void log_resume(void);

/* Add a record with printf formatting. 0, or -1 if the log isn't up. */
int log_write(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int log_vwrite(int level, const char *tag, const char *fmt, va_list ap);

/* Open LOG_DEV for reading and writing (flags: 0 or O_NONBLOCK), at the
 * oldest record kept. The descriptor, or -1. Inline, so that a program's
 * descriptor goes through its own open() and read(). */
static inline int log_open(int flags) {
  return open(LOG_DEV, O_RDWR | (flags & O_NONBLOCK));
}

/* Read the next record into `rec`: 1, or -1 on an error or, under
 * O_NONBLOCK, when none is waiting (select() tells which). */
static inline int log_read(int fd, log_rec_t *rec) {
  return read(fd, rec, sizeof(*rec)) > 0 ? 1 : -1;
}

/* The level's letter (LOG_LEVELS), '?' if out of range. */
char log_level_char(int level);

#ifdef __cplusplus
}
#endif

#endif
