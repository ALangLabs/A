#ifndef _ASTRA_DIRENT_H
#define _ASTRA_DIRENT_H

/* Layout matches glibc x86_64 struct dirent. */
struct dirent {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct _IO_DIR DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
struct dirent *readdir64(DIR *dirp);
int closedir(DIR *dirp);
DIR *fdopendir(int fd);
int dirfd(DIR *dirp);
void rewinddir(DIR *dirp);
long telldir(DIR *dirp);
void seekdir(DIR *dirp, long loc);

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

#endif /* _ASTRA_DIRENT_H */
