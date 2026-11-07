#ifndef _ASTRA_UNISTD_H
#define _ASTRA_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path);
int access(const char *path, int mode);
int faccessat(int dirfd, const char *path, int mode, int flags);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
int isatty(int fd);
long sysconf(int name);

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_CLK_TCK 2

#endif /* _ASTRA_UNISTD_H */
