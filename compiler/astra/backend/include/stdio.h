#ifndef _ASTRA_STDIO_H
#define _ASTRA_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct _IO_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int getc(FILE *f);
int ungetc(int c, FILE *f);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
char *fgets(char *s, int size, FILE *f);
int ferror(FILE *f);
int feof(FILE *f);
int fflush(FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
void clearerr(FILE *f);
int remove(const char *path);
int rename(const char *old, const char *new);
FILE *tmpfile(void);
char *tmpnam(char *s);
int fileno(FILE *f);
FILE *fdopen(int fd, const char *mode);
void perror(const char *s);

#endif /* _ASTRA_STDIO_H */
