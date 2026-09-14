#ifndef FICC_UNISTD_H
#define FICC_UNISTD_H

#include <stddef.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

typedef long ssize_t;

int close(int fd);
int isatty(int fd);
int access(const char *path, int mode);
int unlink(const char *path);
int link(const char *oldpath, const char *newpath);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int mkstemp(char *template_);
int getpid(void);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int rmdir(const char *path);
int mkdir(const char *path, unsigned int mode);

#endif