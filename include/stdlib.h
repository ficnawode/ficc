#ifndef FICC_STDLIB_H
#define FICC_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int status);
void abort(void);
int atoi(const char *nptr);
int abs(int j);
char *getenv(const char *name);

#endif