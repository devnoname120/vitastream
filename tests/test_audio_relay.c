#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2kern/kernel/dmac.h>
#include <psp2kern/kernel/sysmem.h>
#include "audio_relay.h"
#include "firmware.h"

static unsigned char storage[4096] __attribute__((aligned(64)));
static unsigned char pcm[2048], played[2048];
static struct {
	int route_error, fail_alloc, fail_base, fail_op, fail_assign, fail_setup, fail_enqueue;
	int fail_sync, fail_quit, fail_free_op, fail_free_memory;
	int memory_live, op_live, running, setup, allocs, frees, op_frees, quits, enqueues;
	struct firmware_audio_output route;
	SceKernelDmaOpTag tags[2];
} mock;

int firmware_audio_data(uintptr_t *address)
{
	*address = 0x10000000; return mock.route_error ? -70 : 0;
}
int firmware_audio_output(uintptr_t address, struct firmware_audio_output *output)
{
	assert(address == 0x10000000);
	if (mock.route_error) return -70;
	*output = mock.route; return 0;
}
SceUID ksceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
	SceSize size, SceKernelAllocMemBlockKernelOpt *options)
{
	assert(!strcmp(name, "vitastream_local_pcm"));
	assert(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW && size == sizeof(storage));
	assert(options->size == sizeof(*options) && options->alignment == 4096);
	assert(options->attr == (SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT));
	assert(!mock.memory_live); mock.allocs++;
	if (mock.fail_alloc) return -71;
	mock.memory_live = 1; return 5;
}
int ksceKernelGetMemBlockBase(SceUID id, void **base)
{
	assert(id == 5 && mock.memory_live);
	if (mock.fail_base) return -72;
	*base = storage; return 0;
}
int ksceKernelFreeMemBlock(SceUID id)
{
	assert(id == 5 && mock.memory_live && !mock.running && !mock.op_live);
	if (mock.fail_free_memory) return -80;
	mock.memory_live = 0; mock.frees++; return 0;
}
SceKernelDmaOpId ksceKernelDmaOpAlloc(const char *name)
{
	assert(!strcmp(name, "vitastream_local_i2s") && !mock.op_live);
	if (mock.fail_op) return -73;
	mock.op_live = 1; return 7;
}
int ksceKernelDmaOpAssign(SceKernelDmaOpId id, SceKernelDmacId controller, SceUInt32 channel)
{
	assert(id == 7 && mock.op_live && controller == SCE_KERNEL_DMAC_ID_DMAC4 && channel == 15);
	return mock.fail_assign ? -74 : 0;
}
int ksceKernelDmaOpSetupChain(SceKernelDmaOpId id, SceKernelDmaOpTag *tags,
	SceKernelDmaOpChainParam *parameters, SceKernelDmaOpFlag flags)
{
	assert(id == 7 && mock.op_live && !mock.running);
	assert(flags == SCE_KERNEL_DMA_OP_VIRTUAL_SRC_ADDR);
	assert(parameters->size == sizeof(*parameters) && parameters->coherencyMask == 0x3FFFF && !parameters->setValue);
	assert(tags[0].src == storage && tags[1].src == storage + 1920);
	assert(tags[0].src != pcm && !memcmp(storage, pcm, sizeof(pcm)));
	assert((uintptr_t)tags[0].dst == mock.route.fifo && tags[1].dst == tags[0].dst);
	assert(tags[0].len == 0x50000780 && tags[1].len == 0x50000080);
	assert(tags[0].cmd == mock.route.dma_request && tags[1].cmd == (mock.route.dma_request | 0x2000));
	assert(!(tags[0].cmd & 0x7C000)); /* no destructive mixer-source request */
	assert(tags[0].blockSize == 0x800000 && tags[1].blockSize == 0x800000);
	assert(tags[0].pNext == &tags[1] && tags[1].pNext == SCE_KERNEL_DMAC_CHAIN_END);
	if (mock.fail_setup) return -75;
	mock.tags[0] = tags[0]; mock.tags[1] = tags[1]; mock.setup = 1; return 0;
}
int ksceKernelDmaOpEnQueue(SceKernelDmaOpId id)
{
	assert(id == 7 && mock.op_live && mock.setup); mock.enqueues++;
	if (mock.fail_enqueue) return -76;
	mock.running = 1; return 0;
}
int ksceKernelDmaOpSync(SceKernelDmaOpId id, int mode, SceUInt32 timeout_us, SceKernelDmaOpTag **tag)
{
	assert(id == 7 && mock.op_live && mock.running);
	assert(mode == SCE_KERNEL_DMA_OP_SYNC_TIMED_WAIT && timeout_us == 100000 && tag == NULL);
	if (mock.fail_sync) return -77;
	memcpy(played, mock.tags[0].src, 1920);
	memcpy(played + 1920, mock.tags[1].src, 128);
	mock.running = 0; return 0;
}
int ksceKernelDmaOpDeQueue(SceKernelDmaOpId id) {assert(id == 7 && mock.op_live); return 0;}
int ksceKernelDmaOpQuit(SceKernelDmaOpId id)
{
	assert(id == 7 && mock.op_live); mock.quits++;
	if (mock.fail_quit) return -78;
	mock.running = mock.setup = 0; return 0;
}
int ksceKernelDmaOpFree(SceKernelDmaOpId id)
{
	assert(id == 7 && mock.op_live && !mock.running);
	if (mock.fail_free_op) return -79;
	mock.op_live = 0; mock.setup = 0; mock.op_frees++; return 0;
}
static void reset(void)
{
	assert(audio_relay_stop() == 0);
	memset(&mock, 0, sizeof(mock));
	mock.route = (struct firmware_audio_output){0xE0490100, 0xA00000};
	for (unsigned int i = 0; i < sizeof(pcm); i++) pcm[i] = (unsigned char)(i * 19);
}
int main(void)
{
	reset(); mock.route_error = 1;
	assert(audio_relay_start() == -70 && !mock.allocs);
	for (int failure = 0; failure < 4; failure++) {
		reset(); mock.fail_alloc = failure == 0; mock.fail_base = failure == 1;
		mock.fail_op = failure == 2; mock.fail_assign = failure == 3;
		assert(audio_relay_start() < 0 && !mock.memory_live && !mock.op_live);
	}
	reset(); assert(audio_relay_start() == 0);
	assert(audio_relay_write(NULL, 2048) < 0 && audio_relay_write(pcm, 1024) < 0);
	assert(audio_relay_write(pcm, 2048) == 0 && !memcmp(played, pcm, 2048) && !audio_relay_pending());
	mock.route = (struct firmware_audio_output){0xE0470100, 0x900000};
	assert(audio_relay_write(pcm, 2048) == 0 && !memcmp(played, pcm, 2048));
	mock.route_error = 1;
	assert(audio_relay_write(pcm, 2048) == -70 && mock.enqueues == 2);
	assert(audio_relay_stop() == 0 && !mock.memory_live && !mock.op_live);
	for (int failure = 0; failure < 3; failure++) {
		reset(); assert(audio_relay_start() == 0);
		mock.fail_setup = failure == 0; mock.fail_enqueue = failure == 1; mock.fail_sync = failure == 2;
		assert(audio_relay_write(pcm, 2048) < 0);
		assert(audio_relay_pending() == (failure != 0));
		mock.fail_quit = failure != 0;
		if (failure) {
			assert(audio_relay_stop() == -78 && mock.memory_live && mock.op_live);
			assert(audio_relay_write(pcm, 2048) < 0); /* no reuse before the fence */
		}
		mock.fail_quit = 0;
		assert(audio_relay_stop() == 0 && !mock.memory_live && !mock.op_live);
	}
	reset(); assert(audio_relay_start() == 0); mock.fail_free_op = 1;
	assert(audio_relay_stop() == -79 && mock.memory_live && mock.op_live);
	mock.fail_free_op = 0; mock.fail_free_memory = 1;
	assert(audio_relay_stop() == -80 && mock.memory_live && !mock.op_live);
	mock.fail_free_memory = 0;
	assert(audio_relay_stop() == 0 && !mock.memory_live);
	puts("audio relay: PCM identity, native I2S fields, route changes, timeout fences and failure retention passed");
	return 0;
}
