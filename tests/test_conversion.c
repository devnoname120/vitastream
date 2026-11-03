#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int SceSize;
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SCE_UDCD_ERROR_INVALID_ARGUMENT (-2)
#define SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 0
#define SCE_DISPLAY_PIXELFORMAT_BGRA5551 0x50000000
#define SCE_IFTU_PIXELFORMAT_BGRX8888 0x10
#define SCE_IFTU_PIXELFORMAT_RGBX8888 0x20
#define SCE_IFTU_PIXELFORMAT_BGRA1010102 0x40
#define SCE_IFTU_PIXELFORMAT_RGBA1010102 0x80
#define SCE_IFTU_PIXELFORMAT_BGRA5551 4
#define SCE_IFTU_PIXELFORMAT_NV12 0x10000
typedef struct {
	int pid;
	uintptr_t paddr;
	struct {void *base; unsigned int width, height, pitch, pixelformat;} framebuf;
} SceDisplayFrameBufInfo;
typedef struct {
	unsigned int pixelformat, width, height, leftover_stride, leftover_align;
	uintptr_t paddr0, paddr1, paddr2;
} SceIftuFrameBuf;
typedef struct {
	SceIftuFrameBuf fb;
	unsigned int reserved[3], src_w, src_h, dst_x, dst_y, src_x, src_y;
	unsigned int crop_top, crop_bottom, crop_left, crop_right;
} SceIftuPlaneState_updated;
typedef SceIftuPlaneState_updated SceIftuPlaneState;
typedef struct {unsigned int a, b, c, d, e, f; unsigned int matrix[3][3];} SceIftuCscParams;
typedef struct {
	unsigned int size, unk04;
	SceIftuCscParams *csc_params1, *csc_params2;
	unsigned int csc_control, unk14, unk18, unk1C, alpha, unk24;
} SceIftuConvParams;

static SceDisplayFrameBufInfo fb;
static struct {
	int process_refs, range_refs, references, locks, unlocks, releases, translations, conversions;
	int fail_ref, fail_lock, fail_translation, fail_conversion, replace_surface, scatter_surface;
	SceSize size;
} mock;
static int ksceGUIDReferObject(int pid, void *object)
{
	assert(pid == fb.pid && object == NULL);
	mock.references++;
	if (mock.fail_ref) return -71;
	mock.process_refs++; return 0;
}
static int ksceGUIDReleaseObject(int pid)
{
	assert(pid == fb.pid && mock.process_refs == 1 && !mock.range_refs);
	mock.process_refs--; mock.releases++; return 0;
}
static int ksceKernelLockRangeProc(int pid, const void *base, SceSize size)
{
	assert(pid == fb.pid && base == fb.framebuf.base && mock.process_refs == 1 && size);
	mock.locks++;
	if (mock.fail_lock) return -72;
	mock.range_refs++; mock.size = size; return 0;
}
static int ksceKernelUnlockRangeProc(int pid, const void *base, SceSize size)
{
	assert(pid == fb.pid && base == fb.framebuf.base && size == mock.size);
	assert(mock.process_refs == 1 && mock.range_refs == 1);
	mock.range_refs--; mock.unlocks++; return 0;
}
static int ksceKernelProcModeVAtoPA(int pid, unsigned int mode, const void *va, void **pa)
{
	assert(pid == fb.pid && mode == 0x10 && mock.range_refs == 1 && mock.process_refs == 1);
	mock.translations++;
	if (mock.translations == mock.fail_translation) return -73;
	uintptr_t offset = (uintptr_t)va - (uintptr_t)fb.framebuf.base;
	assert(offset == 0 || offset == mock.size - 1);
	*pa = (void *)(fb.paddr + offset + (mock.replace_surface ? 4096 : 0) +
		(mock.scatter_surface && offset ? 4096 : 0));
	return 0;
}
static int ksceIftuCsc(SceIftuFrameBuf *dst, SceIftuPlaneState *src, SceIftuConvParams *params)
{
	assert(mock.process_refs == 1 && mock.range_refs == 1);
	assert(src->fb.paddr0 == fb.paddr && src->fb.width >= fb.framebuf.width);
	assert(src->fb.height >= fb.framebuf.height && dst->pixelformat == SCE_IFTU_PIXELFORMAT_NV12);
	assert(src->crop_right == src->fb.width - fb.framebuf.width);
	assert(src->crop_bottom == src->fb.height - fb.framebuf.height);
	assert(params->alpha == 255 && params->csc_control == 1);
	mock.conversions++;
	return mock.fail_conversion ? -74 : 0;
}

#include "conversion.inc"

static void reset(void)
{
	memset(&mock, 0, sizeof(mock));
	fb = (SceDisplayFrameBufInfo){.pid = 123, .paddr = 0x20000000,
		.framebuf = {.base = (void *)0x98000000, .width = 960, .height = 544, .pitch = 1024}};
}
static void rejected(void)
{
	assert(frame_convert_to_nv12(0x21000000, &fb, 960, 544) < 0);
	assert(!mock.references && !mock.conversions);
}
int main(void)
{
	const unsigned int formats[] = {0, 0x00100000, 0x60800000, 0x60900000, 0x50000000};
	for (unsigned int i = 0; i < sizeof(formats)/sizeof(formats[0]); i++) {
		reset(); fb.framebuf.pixelformat = formats[i];
		assert(frame_convert_to_nv12(0x21000000, &fb, 960, 544) == 0);
		assert(mock.conversions == 1 && mock.unlocks == 1 && mock.releases == 1);
		assert(!mock.process_refs && !mock.range_refs);
	}
	reset(); fb.framebuf.width = 17; fb.framebuf.height = 9; fb.framebuf.pitch = 64;
	assert(frame_convert_to_nv12(0x21000000, &fb, 480, 272) == 0);
	assert(mock.size == 64 * 16 * 4);
	reset(); fb.framebuf.width = 641; fb.framebuf.pitch = 641; rejected();
	reset(); fb.framebuf.pixelformat = 0xDEADBEEF; rejected();
	reset(); fb.framebuf.width = 0; rejected();
	reset(); fb.framebuf.height = 0; rejected();
	reset(); fb.framebuf.height = UINT32_MAX; rejected();
	reset(); fb.framebuf.width = UINT32_MAX; rejected();
	reset(); fb.framebuf.pitch = UINT32_MAX; rejected();
	reset(); fb.framebuf.base = (void *)(UINTPTR_MAX - 7); rejected();
	reset(); fb.paddr = UINTPTR_MAX - 7; rejected();
	reset(); fb.pid = -1; rejected();
	for (int failure = 0; failure < 7; failure++) {
		reset();
		mock.fail_ref = failure == 0;
		mock.fail_lock = failure == 1;
		mock.fail_translation = failure == 2 ? 1 : failure == 3 ? 2 : 0;
		mock.replace_surface = failure == 4;
		mock.scatter_surface = failure == 5;
		mock.fail_conversion = failure == 6;
		assert(frame_convert_to_nv12(0x21000000, &fb, 960, 544) < 0);
		assert(!mock.process_refs && !mock.range_refs);
		assert(mock.conversions == (failure == 6));
	}
	puts("conversion: formats, padded geometry, source leases, replacement/exit and failure cleanup passed");
	return 0;
}
