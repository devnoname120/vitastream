#ifndef VITA_TEST_SUSPEND_H
#define VITA_TEST_SUSPEND_H
#include "vita_test_kernel.h"
typedef enum {
	SCE_KERNEL_POWER_TICK_DEFAULT = 0,
	SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND = 1
} SceKernelPowerTickType;
int ksceKernelPowerTick(SceKernelPowerTickType type);
#endif
