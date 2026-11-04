#ifndef VITA_TEST_FAST_MUTEX_H
#define VITA_TEST_FAST_MUTEX_H
#include <pthread.h>
typedef pthread_mutex_t SceKernelFastMutex;
int ksceKernelInitializeFastMutex(SceKernelFastMutex *, const char *, unsigned int, void *);
int ksceKernelFinalizeFastMutex(SceKernelFastMutex *);
int ksceKernelLockFastMutex(SceKernelFastMutex *);
int ksceKernelUnlockFastMutex(SceKernelFastMutex *);
#endif
