#ifndef VITA_TEST_TAIHEN_H
#define VITA_TEST_TAIHEN_H
#include <psp2kern/types.h>
#define KERNEL_PID 0x10005
typedef struct {
	unsigned int size;
	SceUID modid;
	uint32_t module_nid;
} tai_module_info_t;
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info);
#endif
