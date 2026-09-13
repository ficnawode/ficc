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

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vprintf(const char *format, va_list ap);
int vfprintf(FILE *stream, const char *format, va_list ap);
int puts(const char *s);
int fputs(const char *s, FILE *stream);
int putchar(int c);
int fputc(int c, FILE *stream);
void perror(const char *s);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fflush(FILE *stream);

#endif