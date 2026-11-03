#ifndef VITA_TEST_FCNTL_H
#define VITA_TEST_FCNTL_H
#include "vita_test_kernel.h"
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 0x0200
#define SCE_O_EXCL 0x0800
SceUID ksceIoOpen(const char *path, int flags, int mode);
int ksceIoRead(SceUID fd, void *data, SceSize size);
int ksceIoWrite(SceUID fd, const void *data, SceSize size);
int ksceIoClose(SceUID fd);
#endif
