#ifndef FICC_STDLIB_H
#define FICC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int status);
int atexit(void (*function)(void));
void abort(void);
int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
float strtof(const char *str, char **endptr);
double strtod(const char *str, char **endptr);
long double strtold(const char *str, char **endptr);
long strtol(const char *str, char **endptr, int base);
long long strtoll(const char *str, char **endptr, int base);
unsigned long strtoul(const char *str, char **endptr, int base);
unsigned long long strtoull(const char *str, char **endptr, int base);
int abs(int j);
long labs(long j);
long long llabs(long long j);
char *getenv(const char *name);
int system(const char *command);
int rand(void);
void srand(unsigned int seed);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

#endif