#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_camera.h"
#include <taihen.h>

enum { DATA = 0x10000, STATE = 0x20000, UID = 41, MUTEX = 77, BYTES = 3686400 };
static unsigned char text[0x7000];
static uint32_t words[256];
static void *backing;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int bad_identity, bad_size, bad_signature, bad_pages, fail_hook;
	int hooks, unlocked, get_info, paddr_calls;
	int data_uid;
	void (*on_info)(void);
	atomic_int cancels, wakes, continued, freed, hold_control, release_control;
} native;

static uint32_t read_native(uintptr_t address)
{
	if (address == DATA) return native.data_uid;
	if (address == DATA + 8) return STATE;
	assert(address >= STATE && address < STATE + sizeof(words) && !(address & 3));
	return words[(address - STATE) / 4];
}
#define VIDEO_CAMERA_READ32(address) read_native(address)
static int continue_native(tai_hook_ref_t ref, ...)
{
	va_list args; va_start(args, ref);
	int first = va_arg(args, int);
	if (ref == 1) {
		assert(first == MUTEX && va_arg(args, int) == 1);
		(void)va_arg(args, SceUInt32 *);
		assert(pthread_mutex_lock(&control) == 0);
		atomic_fetch_add(&native.continued, 1);
	} else if (ref == 2) {
		assert(first == UID || first == UID + 1); atomic_fetch_add(&native.freed, 1);
	} else {
		assert(ref == 3); (void)va_arg(args, int);
		atomic_fetch_add(&native.continued, 1);
	}
	va_end(args); return 0;
}
#undef TAI_CONTINUE
#define TAI_CONTINUE(type, ref, ...) ((type)continue_native((ref), __VA_ARGS__))

int ksceKernelTryLockMutex(SceUID id, int count)
{ assert(id == MUTEX && count == 1); return pthread_mutex_trylock(&control) ? -1 : 0; }
int ksceKernelUnlockMutex(SceUID id, int count)
{ assert(id == MUTEX && count == 1); native.unlocked++; return pthread_mutex_unlock(&control); }
int ksceKernelDelayThread(SceUInt32 duration) { assert(duration == 1000); sched_yield(); return 0; }
int ksceKernelSetEventFlag(SceUID id, unsigned int bits)
{ assert(id == 99 && bits == 1); atomic_fetch_add(&native.wakes, 1); return 0; }
void uvc_video_cancel(void) { atomic_fetch_add(&native.cancels, 1); }
int ksceKernelGetMemBlockBase(SceUID id, void **out)
{ assert(id == UID || id == UID + 1); *out = backing; return 0; }
int ksceKernelGetPaddr(const void *p, uintptr_t *out)
{
	uintptr_t offset = (const unsigned char *)p - (const unsigned char *)backing;
	native.paddr_calls++; *out = 0x50000000 + offset;
	if (native.bad_pages && offset == 4096) *out += 4096;
	return 0;
}
static int get_info(SceUID id, unsigned int level, void *info)
{
	uint32_t *v = info; assert((id == UID || id == UID + 1) && level == 7 && v[0] == 184);
	native.get_info++; v[1] = 0x30608006; v[4] = (uint32_t)(uintptr_t)backing;
	v[5] = native.bad_size ? 4096 : BYTES; v[12] = 1;
	if (native.on_info) {
		void (*callback)(void) = native.on_info;
		native.on_info = NULL;
		callback();
	}
	return 0;
}
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *out)
{
	assert(pid == KERNEL_PID);
	out->modid = 12;
	out->module_nid = !strcmp(name, "SceCamera") ? 0xEC26CAF9 : 0x3380B323;
	if (native.bad_identity) out->module_nid = 0;
	return 0;
}
int module_get_offset(SceUID pid, SceUID mod, int segment, size_t offset, uintptr_t *out)
{
	assert(pid == KERNEL_PID && mod == 12);
	if (segment && offset > 0x9D8) return -1;
	*out = (segment ? DATA : (uintptr_t)text) + offset; return 0;
}
int module_get_export_func(SceUID pid, const char *name, uint32_t lib, uint32_t nid, uintptr_t *out)
{
	assert(pid == KERNEL_PID && !strcmp(name, "SceSysmem"));
	assert(lib == 0x6F25E18A && nid == 0xA73CFFEF); *out = (uintptr_t)get_info; return 0;
}
SceUID taiHookFunctionImportForKernel(SceUID pid, tai_hook_ref_t *ref,
	const char *name, uint32_t lib, uint32_t nid, const void *hook)
{
	(void)hook; assert(pid == KERNEL_PID && !strcmp(name, "SceCamera"));
	assert((lib == 0xE2C40624 && nid == 0x16AC80C5) ||
		(lib == 0x6F25E18A && nid == 0x009E1C61));
	if (++native.hooks == native.fail_hook) return -77;
	*ref = lib == 0xE2C40624 ? 1 : 2; return 100 + native.hooks;
}
SceUID taiHookFunctionOffsetForKernel(SceUID pid, tai_hook_ref_t *ref,
	SceUID mod, int segment, size_t offset, int thumb, const void *hook)
{
	(void)hook; assert(pid == KERNEL_PID && mod == 12 && !segment && offset == 0x3C30 && thumb);
	if (++native.hooks == native.fail_hook) return -77;
	*ref = 3; return 100 + native.hooks;
}
#include "../src/video_camera.c"

static void reset(void)
{
	assert(!atomic_load(&g_held));
	memset(&native, 0, sizeof(native)); memset(words, 0, sizeof(words));
	memset(text, 0, sizeof(text));
	memcpy(text + 0x53F0, (unsigned char[]){0x2d,0xe9,0xf0,0x4f}, 4);
	memcpy(text + 0x3C30, (unsigned char[]){0xf0,0xb5}, 2);
	memcpy(text + 0x3C3A, (unsigned char[]){0x83,0xb0,0x05,0x46,0x23,0x68}, 6);
	words[0] = 1; words[2] = MUTEX;
	native.data_uid = UID;
	g_camera_data = 0; g_camera_mutex = -1; g_hooks = 0;
	g_backing_uid = -1; g_backing_base = NULL; g_backing_physical = 0;
	atomic_store(&g_installation, 0);
	g_logged_loan = 0;
	g_lease_generation = g_validated_backing_generation = 0;
	atomic_store(&g_revocation_generation, 0);
	atomic_store(&g_backing_generation, 0);
	atomic_store(&g_native_requests, 0); atomic_store(&g_held, 0);
	video_camera_set_wake_event(99);
}
static void *request_control(void *unused)
{
	(void)unused; assert(camera_lock(MUTEX, 1, NULL) == 0);
	while (atomic_load(&native.hold_control) && !atomic_load(&native.release_control))
		sched_yield();
	assert(ksceKernelUnlockMutex(MUTEX, 1) == 0); return NULL;
}
static void *request_free(void *unused)
{ (void)unused; assert(camera_free(UID) == 0); return NULL; }
static void free_during_validation(void)
{
	/* No lease is published yet, and native code already cleared the UID. */
	native.data_uid = -1;
	assert(camera_free(UID) == 0);
}
int main(void)
{
	backing = aligned_alloc(4096, BYTES); assert(backing);
	struct uvc_video_buffer buffer;
	reset(); assert(video_camera_acquire(0xC0000, &buffer) == 0);
	assert(buffer.frame == backing && buffer.data_paddr == 0x50000010);
	assert(native.hooks == 3 && native.paddr_calls == 901 && video_camera_prepare_unload() < 0);
	assert(video_camera_acquire(0xC0000, &buffer) < 0); video_camera_release();
	assert(!atomic_load(&g_held));
	assert(video_camera_acquire(0xC0000, &buffer) == 0 && native.paddr_calls == 901);
	video_camera_release();
	reset(); assert(video_camera_prepare_unload() == 0);
	assert(video_camera_acquire(0xC0000, &buffer) < 0 && !native.hooks);

	reset(); native.bad_identity = 1;
	assert(video_camera_acquire(0xC0000, &buffer) < 0 && !native.hooks);
	assert(video_camera_prepare_unload() == 0);
	reset(); text[0x3C30] = 0;
	assert(video_camera_acquire(0xC0000, &buffer) < 0 && !native.hooks);
	for (int fail = 1; fail <= 3; fail++) {
		reset(); native.fail_hook = fail;
		assert(video_camera_acquire(0xC0000, &buffer) < 0);
		assert((video_camera_prepare_unload() < 0) == (fail > 1));
	}
	for (unsigned int i = 0; i < 8; i++) {
		static const int guard[] = {6,7,9,26,126,246,127,247};
		reset(); words[guard[i]] = 1;
		assert(video_camera_acquire(0xC0000, &buffer) < 0 && !atomic_load(&g_held));
		assert(native.unlocked == 1);
		words[guard[i]] = 0;
		assert(video_camera_acquire(0xC0000, &buffer) == 0);
		video_camera_release();
	}
	reset(); native.bad_size = 1;
	assert(video_camera_acquire(0xC0000, &buffer) < 0 && native.unlocked == 1);
	reset(); native.bad_pages = 1;
	assert(video_camera_acquire(0xC0000, &buffer) < 0 && native.unlocked == 1);

	/* A camera request cancels/wakes capture, then waits at the native mutex.
	 * Only the thread which owns the lease returns that mutex. */
	reset(); atomic_store(&native.hold_control, 1);
	assert(video_camera_acquire(0xC0000, &buffer) == 0);
	pthread_t thread; assert(pthread_create(&thread, NULL, request_control, NULL) == 0);
	while (!atomic_load(&native.cancels)) sched_yield();
	assert(video_camera_revoked() && !atomic_load(&native.continued));
	video_camera_release();
	assert(video_camera_acquire(0xC0000, &buffer) < 0);
	atomic_store(&native.release_control, 1);
	assert(pthread_join(thread, NULL) == 0);
	assert(atomic_load(&native.continued) == 1 && !atomic_load(&g_native_requests));
	assert(video_camera_acquire(0xC0000, &buffer) == 0 && !video_camera_revoked());
	assert(native.paddr_calls == 901); video_camera_release();

	reset(); assert(video_camera_acquire(0xC0000, &buffer) == 0);
	/* Native teardown may clear its UID field before calling FreeMemBlock. */
	native.data_uid = -1;
	assert(pthread_create(&thread, NULL, request_free, NULL) == 0);
	while (!atomic_load(&native.cancels)) sched_yield();
	assert(!atomic_load(&native.freed)); video_camera_release();
	assert(pthread_join(thread, NULL) == 0 && atomic_load(&native.freed) == 1);
	assert(video_camera_acquire(0xC0000, &buffer) < 0);
	/* Recreated backing may even reuse its old UID: invalidate cached pages. */
	void *old_backing = backing;
	backing = aligned_alloc(4096, BYTES); assert(backing);
	native.data_uid = UID;
	assert(video_camera_acquire(0xC0000, &buffer) == 0);
	assert(buffer.frame == backing && native.get_info == 2 && native.paddr_calls == 1802);
	video_camera_release(); free(old_backing);
	/* A changed UID is independently revalidated without reboot. */
	native.data_uid = UID + 1;
	assert(video_camera_acquire(0xC0000, &buffer) == 0 && native.get_info == 3);
	video_camera_release();

	reset(); native.on_info = free_during_validation;
	assert(video_camera_acquire(0xC0000, &buffer) < 0 && !atomic_load(&g_held));
	assert(atomic_load(&native.freed) == 1 && !atomic_load(&g_native_requests));
	native.data_uid = UID;
	assert(video_camera_acquire(0xC0000, &buffer) == 0 && native.get_info == 2);
	video_camera_release();

	reset(); assert(video_camera_acquire(0xC0000, &buffer) == 0);
	assert(camera_event(0, 0x400) == 0 && video_camera_revoked());
	assert(atomic_load(&g_held)); video_camera_release();
	assert(video_camera_acquire(0xC0000, &buffer) == 0 && native.get_info == 2);
	video_camera_release();

	/* Repeated cross-thread handoffs must not turn a temporary revoke into a
	 * permanent disable, nor reuse backing while a native request is pending. */
	reset();
	for (int i = 0; i < 200; i++) {
		assert(video_camera_acquire(0xC0000, &buffer) == 0);
		int cancels = atomic_load(&native.cancels);
		assert(pthread_create(&thread, NULL, request_control, NULL) == 0);
		while (atomic_load(&native.cancels) == cancels) sched_yield();
		assert(video_camera_revoked()); video_camera_release();
		assert(pthread_join(thread, NULL) == 0 && !atomic_load(&g_native_requests));
	}
	assert(native.paddr_calls == 901);
	free(backing);
	puts("camera backend: profile, idle-state, backing and concurrent handoff checks passed");
	return 0;
}
