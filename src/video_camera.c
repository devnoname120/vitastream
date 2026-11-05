/* Firmware-guarded 3.60 camera DRAM borrowing backend. */
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <stdatomic.h>
#include <string.h>
#include "video_camera.h"
#include "diagnostic.h"

int module_get_offset(SceUID, SceUID, int, size_t, uintptr_t *);
int module_get_export_func(SceUID, const char *, uint32_t, uint32_t, uintptr_t *);

/* These addresses are relative to SceCamera's data segment, not fixed VAs. */
enum { CAMERA_BYTES = 3686400, CAMERA_DATA_BYTES = 0x9D8 };
static uintptr_t g_camera_data;
static SceUID g_camera_mutex = -1, g_wake_event = -1;
static atomic_int g_held, g_native_requests;
static atomic_uint g_revocation_generation, g_backing_generation;
/* Only the capture thread changes these two snapshots. */
static unsigned int g_lease_generation, g_validated_backing_generation;
/* 0 unstarted, -1 closing, -2 installing, -3 unavailable; positive = hooks. */
static atomic_int g_installation;
static int g_hooks, g_logged_loan;
static atomic_int g_backing_uid = ATOMIC_VAR_INIT(-1);
static void *g_backing_base;
static uintptr_t g_backing_physical;
static tai_hook_ref_t g_lock_ref, g_free_ref, g_event_ref;
static int (*g_memblock_info)(SceUID, unsigned int, void *);

/* Host tests substitute reads to exercise the actual native-state guards. */
#ifndef VIDEO_CAMERA_READ32
#define VIDEO_CAMERA_READ32(address) (*(const volatile uint32_t *)(address))
#endif
static uint32_t read_word(uintptr_t address)
{
	return VIDEO_CAMERA_READ32(address);
}

static void revoke(void)
{
	atomic_fetch_add(&g_revocation_generation, 1);
	if (atomic_load(&g_held)) {
		/* The capture thread owns the camera mutex and must release it itself.
		 * Cancellation wakes a stalled USB wait; the main event wakes a paused
		 * capture loop. Never unlock a thread-owned mutex from this hook. */
		uvc_video_cancel();
		if (g_wake_event >= 0)
			ksceKernelSetEventFlag(g_wake_event, 1);
	}
}

static int camera_lock(SceUID mutex, int count, SceUInt32 *timeout)
{
	uintptr_t state = g_camera_data ? read_word(g_camera_data + 8) : 0;
	if (!state || mutex != (SceUID)read_word(state + 8))
		return TAI_CONTINUE(int, g_lock_ref, mutex, count, timeout);
	/* Do not let a retry overtake native callers waiting for our lease. Once
	 * LockMutex returns, the native mutex itself protects their operation. */
	atomic_fetch_add(&g_native_requests, 1);
	revoke();
	int ret = TAI_CONTINUE(int, g_lock_ref, mutex, count, timeout);
	atomic_fetch_sub(&g_native_requests, 1);
	return ret;
}

static int camera_free(SceUID uid)
{
	/* Fence every Camera-owned free: native teardown may clear the published
	 * UID before reaching this import, including during first validation. */
	atomic_fetch_add(&g_native_requests, 1);
	atomic_fetch_add(&g_backing_generation, 1);
	revoke();
	while (atomic_load(&g_held))
		ksceKernelDelayThread(1000);
	int ret = TAI_CONTINUE(int, g_free_ref, uid);
	atomic_fetch_sub(&g_native_requests, 1);
	return ret;
}

static int camera_event(int resume, int event)
{
	/* Fence the worker before its suspend notification, not after it wakes. */
	atomic_fetch_add(&g_native_requests, 1);
	atomic_fetch_add(&g_backing_generation, 1);
	revoke();
	/* The native worker and suspend barrier take the same control mutex.
	 * Do not stall the system-event dispatcher waiting for USB callbacks. */
	int ret = TAI_CONTINUE(int, g_event_ref, resume, event);
	atomic_fetch_sub(&g_native_requests, 1);
	return ret;
}

static int initialize(void)
{
	int expected = 0;
	if (!atomic_compare_exchange_strong(&g_installation, &expected, -2))
		return expected == 3 ? 0 : -1;
	tai_module_info_t module = {0};
	module.size = sizeof(module);
	uintptr_t text, text_end, data, data_end, info;
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceCamera", &module) < 0 ||
	    module.module_nid != 0xEC26CAF9u ||
	    module_get_offset(KERNEL_PID, module.modid, 0, 0, &text) < 0 ||
	    module_get_offset(KERNEL_PID, module.modid, 0, 0x66CC, &text_end) < 0 ||
	    module_get_offset(KERNEL_PID, module.modid, 1, 0, &data) < 0 ||
	    module_get_offset(KERNEL_PID, module.modid, 1, CAMERA_DATA_BYTES, &data_end) < 0 ||
	    text_end - text != 0x66CC || data_end - data != CAMERA_DATA_BYTES ||
	    !text || !data || (data & 3u))
		goto profile_unavailable;
	/* Relocated MOVW/MOVT immediates are excluded from these code checks. */
	static const unsigned char worker[] = {0x2d,0xe9,0xf0,0x4f};
	static const unsigned char event[] = {0xf0,0xb5};
	static const unsigned char event_body[] = {0x83,0xb0,0x05,0x46,0x23,0x68};
	if (memcmp((void *)(text + 0x53F0), worker, sizeof(worker)) ||
	    memcmp((void *)(text + 0x3C30), event, sizeof(event)) ||
	    memcmp((void *)(text + 0x3C3A), event_body, sizeof(event_body)) ||
	    module_get_export_func(KERNEL_PID, "SceSysmem", 0x6F25E18A,
		0xA73CFFEF, &info) < 0)
		goto profile_unavailable;
	/* This private ABI is also pinned, independently of SceCamera. */
	tai_module_info_t sysmem = {0};
	sysmem.size = sizeof(sysmem);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceSysmem", &sysmem) < 0 ||
	    sysmem.module_nid != 0x3380B323u)
		goto profile_unavailable;
	g_memblock_info = (void *)info;
	g_camera_data = data;
	int hook = taiHookFunctionImportForKernel(KERNEL_PID, &g_lock_ref,
		"SceCamera", 0xE2C40624, 0x16AC80C5, camera_lock);
	if (hook < 0)
		goto unavailable;
	g_hooks++;
	hook = taiHookFunctionImportForKernel(KERNEL_PID, &g_free_ref,
		"SceCamera", 0x6F25E18A, 0x009E1C61, camera_free);
	if (hook < 0)
		goto unavailable;
	g_hooks++;
	hook = taiHookFunctionOffsetForKernel(KERNEL_PID, &g_event_ref, module.modid,
		0, 0x3C30, 1, camera_event);
	if (hook < 0)
		goto unavailable;
	g_hooks++;
	atomic_store(&g_installation, g_hooks);
	diagnostic_record("camera borrow guards installed", g_hooks);
	return 0;
unavailable:
	atomic_store(&g_installation, g_hooks ? g_hooks : -3);
	diagnostic_record("camera borrow guard failure", hook);
	return -1;
profile_unavailable:
	atomic_store(&g_installation, -3);
	diagnostic_record("camera borrow profile unavailable", -1);
	return -1;
}

void video_camera_set_wake_event(SceUID event)
{
	g_wake_event = event;
}

int video_camera_revoked(void)
{
	return atomic_load(&g_revocation_generation) != g_lease_generation;
}

int video_camera_acquire(unsigned int bytes, struct uvc_video_buffer *buffer)
{
	if (!buffer || !bytes || bytes > CAMERA_BYTES || initialize() < 0 ||
	    atomic_load(&g_native_requests) || atomic_load(&g_held))
		return -1;
	uintptr_t state = read_word(g_camera_data + 8);
	if (!state || (state & 3u))
		return -1;
	SceUID mutex = (SceUID)read_word(state + 8);
	if (mutex < 0 || ksceKernelTryLockMutex(mutex, 1) < 0)
		return -1;
	int ret = -1;
	int stage = 1;
	unsigned int generation = atomic_load(&g_revocation_generation);
	unsigned int backing_generation = atomic_load(&g_backing_generation);
	/* The camera's control mutex fences all start/read/control work. Require
	 * initialized, clocks off, no active channel and no suspend transition. */
	if (atomic_load(&g_native_requests) || read_word(g_camera_data + 8) != state ||
	    read_word(state) != 1 || read_word(state + 6*4) ||
	    read_word(state + 7*4) || read_word(state + 9*4) ||
	    read_word(state + 26*4) ||
	    read_word(state + 126*4) || read_word(state + 246*4) ||
	    read_word(state + 127*4) || read_word(state + 247*4))
		goto out;
	SceUID uid = (SceUID)read_word(g_camera_data);
	stage = 2;
	int cached_uid = atomic_load(&g_backing_uid);
	if (cached_uid >= 0 && uid == cached_uid &&
	    backing_generation == g_validated_backing_generation) {
		goto validated;
	}
	void *base;
	uint32_t details[46] = {sizeof(details)};
	stage = 3;
	if (uid < 0 || ksceKernelGetMemBlockBase(uid, &base) < 0 || !base ||
	    ((uintptr_t)base & 4095u) || g_memblock_info(uid, 7, details) < 0 ||
	    details[4] != (uint32_t)(uintptr_t)base || details[5] != CAMERA_BYTES ||
	    (details[1] != 0x10208006u && details[1] != 0x30608006u) ||
	    details[12] != 1)
		goto out;
	uintptr_t physical, page;
	stage = 4;
	if (ksceKernelGetPaddr(base, &physical) < 0 || !physical)
		goto out;
	/* Validate again after free/reinitialization, even if a UID was reused.
	 * Ordinary control handoffs retain the cache for the same backing. */
	for (unsigned int offset = 0; offset < CAMERA_BYTES; offset += 4096) {
		if (ksceKernelGetPaddr((unsigned char *)base + offset, &page) < 0 ||
		    page != physical + offset)
			goto out;
	}
	atomic_store(&g_backing_uid, uid);
	g_backing_base = base;
	g_backing_physical = physical;
	g_validated_backing_generation = backing_generation;
	diagnostic_record("camera backing validated bytes", CAMERA_BYTES);
validated:
	/* Publish before the last revocation check. A competing camera request
	 * either cancels this lease or finds it revoked before any DMA begins. */
	g_camera_mutex = mutex;
	g_lease_generation = generation;
	atomic_store(&g_held, 1);
	if (video_camera_revoked() || atomic_load(&g_native_requests) ||
	    atomic_load(&g_backing_generation) != backing_generation) {
		atomic_store(&g_held, 0);
		goto out;
	}
	buffer->frame = g_backing_base;
	buffer->data_paddr = g_backing_physical + offsetof(struct uvc_frame, data);
	if (!g_logged_loan) {
		diagnostic_record("camera DRAM borrowed bytes", bytes);
		g_logged_loan = 1;
	}
	return 0;
out:
	{
		static int logged_stage;
		if (stage != logged_stage) {
			diagnostic_record("camera borrow unavailable stage", stage);
			if (stage == 1) {
				diagnostic_record("camera ready", read_word(state));
				diagnostic_record("camera bus clock active", read_word(state + 6*4));
				diagnostic_record("camera peripheral clock active", read_word(state + 7*4));
			}
			logged_stage = stage;
		}
	}
	ksceKernelUnlockMutex(mutex, 1);
	return ret;
}

void video_camera_release(void)
{
	if (!atomic_load(&g_held))
		return;
	/* Called only by the capture thread, after IFTU and USB ownership ended. */
	int ret = ksceKernelUnlockMutex(g_camera_mutex, 1);
	if (ret < 0) {
		diagnostic_record("camera control mutex release failed", ret);
		return;
	}
	atomic_store(&g_held, 0);
	if (video_camera_revoked() && g_logged_loan) {
		diagnostic_record("camera DRAM returned; serial capture", 0);
		g_logged_loan = 0;
	}
}

int video_camera_prepare_unload(void)
{
	/* Import/callback hooks remain resident, as with the audio route guard. */
	int current = atomic_load(&g_installation);
	do {
		if (current > 0 || current == -2 || atomic_load(&g_held))
			return -1;
	} while (!atomic_compare_exchange_weak(&g_installation, &current, -1));
	return 0;
}
