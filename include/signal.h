#ifndef FICC_SIGNAL_H
#define FICC_SIGNAL_H

/* Layout matches glibc's x86-64 <bits/sigaction.h> so ficc objects can
   pass a 'struct sigaction' to libc's sigaction(). */
typedef struct sigset_t
{
    unsigned long __val[16];
} sigset_t;

typedef void (*__sighandler_t)(int);
typedef int sig_atomic_t;

struct sigaction
{
    __sighandler_t sa_handler;
    unsigned long sa_flags;
    sigset_t sa_mask;
    void (*sa_restorer)(void);
};

#define SIG_DFL ((__sighandler_t) 0)
#define SIG_IGN ((__sighandler_t) 1)
#define SIG_ERR ((__sighandler_t) - 1)

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

int sigemptyset(sigset_t *set);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
__sighandler_t signal(int sig, __sighandler_t handler);
int raise(int sig);

#endif