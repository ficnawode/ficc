#ifndef FICC_STDIO_H
#define FICC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct __FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define EOF (-1)

#define BUFSIZ 8192
#define FILENAME_MAX 4096
#define L_tmpnam 32

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vprintf(const char *format, va_list ap);
int vfprintf(FILE *stream, const char *format, va_list ap);

int puts(const char *s);
int fputs(const char *s, FILE *stream);
int putchar(int c);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);

int getc(FILE *stream);
int getchar(void);
int getc_unlocked(FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int ungetc(int c, FILE *stream);

void perror(const char *s);
FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fseeko(FILE *stream, long offset, int whence);
long ftello(FILE *stream);
void rewind(FILE *stream);
int fflush(FILE *stream);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);

void clearerr(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);

FILE *tmpfile(void);
char *tmpnam(char *s);

int remove(const char *path);
int rename(const char *oldpath, const char *newpath);

FILE *popen(const char *command, const char *type);
int pclose(FILE *stream);
int fileno(FILE *stream);

int fscanf(FILE *stream, const char *format, ...);
int scanf(const char *format, ...);
int sscanf(const char *str, const char *format, ...);

void flockfile(FILE *stream);
void funlockfile(FILE *stream);
int ftrylockfile(FILE *stream);

#endif