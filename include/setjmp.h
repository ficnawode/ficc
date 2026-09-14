#ifndef FICC_SETJMP_H
#define FICC_SETJMP_H

/* Matches the glibc x86-64 layout so ficc objects can use libc's
   setjmp/_setjmp and longjmp/_longjmp directly.
   __mask_was_saved + saved mask follow the 64-byte register save area. */
typedef struct __jmp_buf_tag
{
    long __jmpbuf[8];
    int __mask_was_saved;
    unsigned long __saved_mask[16];
} jmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
int _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val);

#endif