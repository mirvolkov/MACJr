#ifndef _FCNTL_H
#define _FCNTL_H

/* newlib/picolibc values, as the firmware uses them */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_APPEND   0x0008
#define O_CREAT    0x0200
#define O_TRUNC    0x0400
#define O_NONBLOCK 0x4000

int open(const char *path, int flags, ...);

#endif
