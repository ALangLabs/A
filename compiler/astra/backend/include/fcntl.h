#ifndef _ASTRA_FCNTL_H
#define _ASTRA_FCNTL_H

#include <stddef.h>
#include <sys/types.h>

/* Minimal subset needed by lib/tcov.c (compile-time declarations only;
 * the symbols resolve against the host libc at link time). */

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400

#define F_UNLCK 2
#define F_RDLCK 0
#define F_WRLCK 1
#define F_SETLKW 7

struct flock {
  short l_type;
  short l_whence;
  off_t l_start;
  off_t l_len;
  pid_t l_pid;
};

int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#endif /* _ASTRA_FCNTL_H */
