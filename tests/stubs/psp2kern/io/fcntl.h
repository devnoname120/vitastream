#ifndef VITA_TEST_FCNTL_H
#define VITA_TEST_FCNTL_H
#include "vita_test_kernel.h"
#define SCE_O_RDONLY 1
SceUID ksceIoOpen(const char *path, int flags, int mode);
int ksceIoRead(SceUID fd, void *data, SceSize size);
int ksceIoClose(SceUID fd);
#endif
