#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

int read(int fd, void *buf, size_t n);
int write(int fd, const void *buf, size_t n);
int close(int fd);
int usleep(unsigned us);
unsigned sleep(unsigned s);

#endif
