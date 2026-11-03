#ifndef VITA_TEST_DMAC_H
#define VITA_TEST_DMAC_H
#include "vita_test_kernel.h"
typedef SceInt32 SceKernelDmaOpId;
typedef int SceKernelDmacId;
typedef int SceKernelDmaOpFlag;
typedef struct SceKernelDmaOpTag {
	const void *src;
	void *dst;
	SceUInt32 len, cmd, keyring;
	void *iv;
	SceUInt32 blockSize;
	struct SceKernelDmaOpTag *pNext;
} SceKernelDmaOpTag;
typedef struct {SceSize size; SceUInt32 coherencyMask, setValue;} SceKernelDmaOpChainParam;
#define SCE_KERNEL_DMAC_ID_DMAC4 0x12
#define SCE_KERNEL_DMA_OP_VIRTUAL_SRC_ADDR 1
#define SCE_KERNEL_DMA_OP_SYNC_TIMED_WAIT 3
#define SCE_KERNEL_DMAC_CHAIN_END ((SceKernelDmaOpTag *)(uintptr_t)0xFFFFFFFF)
SceKernelDmaOpId ksceKernelDmaOpAlloc(const char *name);
int ksceKernelDmaOpAssign(SceKernelDmaOpId op, SceKernelDmacId controller, SceUInt32 channel);
int ksceKernelDmaOpSetupChain(SceKernelDmaOpId op, SceKernelDmaOpTag *tags,
	SceKernelDmaOpChainParam *parameters, SceKernelDmaOpFlag flags);
int ksceKernelDmaOpEnQueue(SceKernelDmaOpId op);
int ksceKernelDmaOpSync(SceKernelDmaOpId op, int mode, SceUInt32 timeout_us, SceKernelDmaOpTag **tag);
int ksceKernelDmaOpDeQueue(SceKernelDmaOpId op);
int ksceKernelDmaOpQuit(SceKernelDmaOpId op);
int ksceKernelDmaOpFree(SceKernelDmaOpId op);
#endif
