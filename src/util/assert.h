#ifndef FICC_ASSERT_H
#define FICC_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#define ASSERT(COND) ((void) 0)
#else
#define ASSERT(COND)                                                                               \
    do                                                                                             \
    {                                                                                              \
        if (!(COND))                                                                               \
        {                                                                                          \
            fprintf(stderr, "ficc: internal error: assertion failed: %s at %s:%d\n", #COND,        \
                    __FILE__, __LINE__);                                                           \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#endif

#endif
