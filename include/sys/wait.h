#ifndef FICC_SYS_WAIT_H
#define FICC_SYS_WAIT_H

#include <sys/types.h>

/* Value returned by wait()/pclose(): low 16 bits describe the exit. */
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) ((((status) & 0xff00) >> 8))
#define WIFSIGNALED(status) (((signed char) (((status) & 0x7f) + 1) >> 1) > 0)
#define WTERMSIG(status) ((status) & 0x7f)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif