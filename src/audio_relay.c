#include <psp2kern/kernel/dmac.h>
#include <psp2kern/kernel/sysmem.h>
#include <stdint.h>
#include <string.h>
#include "audio_relay.h"
#include "diagnostic.h"
#include "firmware.h"

#define RELAY_FRAMES 512u
#define RELAY_BYTES (RELAY_FRAMES * 4u)
#define RELAY_DMA_CHANNEL 15u
#define RELAY_TIMEOUT_US 100000u

/* The firmware takes a timeout value and passes its stack copy to the event
 * wait. Some SDK headers incorrectly declare a timeout pointer. Keep the
 * actual export symbol while giving this call site the verified ABI. */
#define RELAY_SYMBOL_TEXT_(name) #name
#define RELAY_SYMBOL_TEXT(name) RELAY_SYMBOL_TEXT_(name)
extern int relay_dma_sync(SceKernelDmaOpId op, int mode, SceUInt32 timeout_us,
	SceKernelDmaOpTag **error_tag)
	__asm__(RELAY_SYMBOL_TEXT(__USER_LABEL_PREFIX__) "ksceKernelDmaOpSync");

/* Stock SceAudio reserves DMAC4 channels 0..8. Display/codec/security users
 * found in the firmware use the other controller classes. Channel assignment
 * is not exclusive: all cancellation below names only our own operation. */
static SceKernelDmaOpId operation = -1;
static SceUID memory_uid = -1;
static unsigned char *memory;
static int pending;
static unsigned int transfers;
static uintptr_t audio_data;
static SceKernelDmaOpTag tags[2] __attribute__((aligned(64)));

int audio_relay_pending(void)
{
	return pending;
}

int audio_relay_stop(void)
{
	int ret;
	if (operation >= 0) {
		if (pending) {
			/* Dequeue removes only our queued work. Quit fences/aborts only
			 * this operation; no controller/MMIO reset is performed here. */
			(void)ksceKernelDmaOpDeQueue(operation);
			ret = ksceKernelDmaOpQuit(operation);
			diagnostic_record("stop local audio DMA", ret);
			if (ret < 0)
				return ret;
			pending = 0;
		}
		ret = ksceKernelDmaOpFree(operation);
		if (ret < 0)
			return ret;
		operation = -1;
		diagnostic_record("local audio DMA completed blocks", (int)transfers);
	}
	if (memory_uid >= 0) {
		ret = ksceKernelFreeMemBlock(memory_uid);
		if (ret < 0)
			return ret;
		memory_uid = -1;
		memory = NULL;
	}
	audio_data = 0;
	return 0;
}

int audio_relay_start(void)
{
	struct firmware_audio_output output;
	SceKernelAllocMemBlockKernelOpt opt = {0};
	int ret = audio_relay_stop();
	if (ret < 0)
		return ret;
	ret = firmware_audio_data(&audio_data);
	if (ret < 0)
		return ret;
	ret = firmware_audio_output(audio_data, &output);
	if (ret < 0)
		return ret;
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4096;
	memory_uid = ksceKernelAllocMemBlock("vitastream_local_pcm",
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW, 4096, &opt);
	if (memory_uid < 0)
		return memory_uid;
	ret = ksceKernelGetMemBlockBase(memory_uid, (void **)&memory);
	if (ret < 0)
		goto fail;
	operation = ksceKernelDmaOpAlloc("vitastream_local_i2s");
	if (operation < 0) {
		ret = operation;
		goto fail;
	}
	ret = ksceKernelDmaOpAssign(operation, SCE_KERNEL_DMAC_ID_DMAC4,
		RELAY_DMA_CHANNEL);
	if (ret < 0)
		goto fail;
	transfers = 0;
	diagnostic_record("local audio FIFO", (int)output.fifo);
	diagnostic_record("local audio destination request", (int)output.dma_request);
	return 0;
fail:
	(void)audio_relay_stop();
	return ret;
}

int audio_relay_write(const void *pcm, size_t bytes)
{
	static const SceKernelDmaOpChainParam parameters = {
		.size = sizeof(SceKernelDmaOpChainParam),
		.coherencyMask = 0x0003FFFF,
		.setValue = 0
	};
	struct firmware_audio_output output;
	int ret;
	if (!pcm || bytes != RELAY_BYTES || !memory || operation < 0 || pending)
		return -1;
	/* Re-read the selected sink so native routing changes are respected. */
	ret = firmware_audio_output(audio_data, &output);
	if (ret < 0)
		return ret;
	/* A private buffer decouples the native monitor's ping-pong ownership
	 * from playback. This adds only 192 kB/s of copying at 48 kHz stereo. */
	memcpy(memory, pcm, bytes);
	/* Combine Sony's incrementing-RAM source encoding with its I2S sink
	 * handshake and 128-byte destination burst. No source peripheral request
	 * is set, so this operation never reads a SourceMixer FIFO. */
	tags[0] = (SceKernelDmaOpTag){
		.src = memory,
		.dst = (void *)output.fifo,
		.len = 0x50000000u | (RELAY_BYTES - 128u),
		.cmd = output.dma_request,
		.blockSize = 0x00800000u,
		.pNext = &tags[1]
	};
	tags[1] = (SceKernelDmaOpTag){
		.src = memory + RELAY_BYTES - 128u,
		.dst = (void *)output.fifo,
		.len = 0x50000080u,
		.cmd = output.dma_request | 0x2000u,
		.blockSize = 0x00800000u,
		.pNext = SCE_KERNEL_DMAC_CHAIN_END
	};
	ret = ksceKernelDmaOpSetupChain(operation, tags,
		(SceKernelDmaOpChainParam *)&parameters, SCE_KERNEL_DMA_OP_VIRTUAL_SRC_ADDR);
	if (ret < 0)
		return ret;
	/* Retain ownership even if enqueue fails: SetupChain owns descriptors
	 * which must be returned by the stop path before reuse or unload. */
	pending = 1;
	ret = ksceKernelDmaOpEnQueue(operation);
	if (ret < 0)
		return ret;
	ret = relay_dma_sync(operation, SCE_KERNEL_DMA_OP_SYNC_TIMED_WAIT,
		RELAY_TIMEOUT_US, NULL);
	if (ret >= 0) {
		pending = 0;
		if (!transfers)
			diagnostic_record("local audio DMA first completed block", ret);
		transfers++;
	}
	return ret;
}
