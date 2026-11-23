#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

/* The standard streams: all three are the program's screen and keys,
 * /dev/tty. write() to 1 or 2 prints, read() from 0 gives a key code per
 * byte; close() of them does nothing. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int read(int fd, void *buf, size_t n);
int write(int fd, const void *buf, size_t n);
int close(int fd);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
long lseek(int fd, long off, int whence); /* the new offset, -1 on error */
int usleep(unsigned us);
unsigned sleep(unsigned s);

#endif
