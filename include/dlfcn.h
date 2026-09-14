#ifndef FICC_DLFCN_H
#define FICC_DLFCN_H

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_BINDING_MASK 3
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_NODELETE 0x1000
#define RTLD_NOLOAD 4
#define RTLD_DEEPBIND 8

#define RTLD_DEFAULT ((void *) 0)
#define RTLD_NEXT ((void *) -1)

void *dlopen(const char *filename, int flags);
int dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);

#endif