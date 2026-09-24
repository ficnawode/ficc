#ifndef FICC_SYS_STAT_H
#define FICC_SYS_STAT_H

#include <sys/types.h>

typedef unsigned int mode_t;

int chmod(const char *path, mode_t mode);

#endif
