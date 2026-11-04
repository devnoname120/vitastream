#include <psp2kern/types.h>
#include <psp2kern/udcd.h>
#include <psp2kern/kernel/threadmgr/fast_mutex.h>
#include <taihen.h>
#include <stdatomic.h>
#include <stdint.h>
#include "audio_route.h"
#include "firmware.h"
#include "diagnostic.h"

enum { GUARD_IDLE = -1, GUARD_INSTALLING = -2, GUARD_CLOSING = -3 };
static atomic_int g_guard_id = ATOMIC_VAR_INIT(GUARD_IDLE);
static atomic_uint g_guard_taken, g_guard_held;
static SceKernelFastMutex g_route_mutex;
static int g_route_initialized, g_guard_owned;
static uintptr_t g_native_data;
static tai_hook_ref_t g_guard_ref;
static struct {
	int owned;
	int restore_mode;
} g_paths[2];
static const unsigned int g_mode_offsets[2] = {0xBB28u, 0xB9BCu};

static int native_mode(int path)
{
	return *(const volatile signed char *)(g_native_data + g_mode_offsets[path]);
}

int audio_route_guard_init(void)
{
	if (g_route_initialized)
		return 0;
	int ret = ksceKernelInitializeFastMutex(&g_route_mutex, "uac_audio_routes", 0, NULL);
	if (ret >= 0)
		g_route_initialized = 1;
	return ret;
}

void audio_route_guard_term(void)
{
	/* Called only after the unload gate and both audio workers have stopped,
	 * or during startup rollback before a hook could have been published. */
	if (g_route_initialized) {
		ksceKernelFinalizeFastMutex(&g_route_mutex);
		g_route_initialized = 0;
	}
}

static int avconfig_set_monitor_mode(int path, int mode)
{
	if (path != 0 && path != 1)
		return TAI_CONTINUE(int, g_guard_ref, path, mode);
	/* Only SceAVConfig's thread-context setter calls enter this hook. The
	 * native setter never calls back into it. Share this lock with capture
	 * claims/drains/restores so a newer request cannot be overwritten by
	 * cleanup after its ownership check. No DMA callback takes this lock. */
	ksceKernelLockFastMutex(&g_route_mutex);
	if (mode == 0 && g_paths[path].owned) {
		if (g_guard_owned && native_mode(0) == 2 && native_mode(1) == 2) {
			g_paths[path].restore_mode = 0;
			atomic_fetch_add(&g_guard_held, 1);
			ksceKernelUnlockFastMutex(&g_route_mutex);
			return 0;
		}
	}
	int ret = TAI_CONTINUE(int, g_guard_ref, path, mode);
	if (ret >= 0 && mode == 0 && g_paths[path].owned)
		g_paths[path].restore_mode = 0;
	if (ret >= 0 && mode != 0) {
		if (g_paths[path].owned) {
			g_paths[path].owned = 0;
			atomic_fetch_or(&g_guard_taken, 1u << path);
		}
		g_guard_owned = 0;
	}
	ksceKernelUnlockFastMutex(&g_route_mutex);
	return ret;
}

int audio_route_guard_capture_mode(int path, uintptr_t data, AudioRouteSetModeFn set_mode)
{
	if (!g_route_initialized || !data || path < 0 || path > 1 || !set_mode)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;
	ksceKernelLockFastMutex(&g_route_mutex);
	int ret = SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
	if (g_native_data && g_native_data != data && (g_paths[0].owned || g_paths[1].owned))
		goto out;
	g_native_data = data;
	int mode = native_mode(path);
	if (path == 0) {
		if (mode != 0 && !(mode == 2 && g_paths[0].owned))
			goto out;
	} else {
		if (!g_paths[0].owned || (atomic_load(&g_guard_taken) & 1u) || native_mode(0) != 2)
			goto out;
		if (mode == 1 || (mode == 2 && !g_paths[1].owned))
			goto out;
	}
	ret = set_mode(path, 2);
	if (ret >= 0) {
		/* Preserve a later blocked request when recovering the same lease. */
		if (!g_paths[path].owned)
			g_paths[path].restore_mode = mode;
		g_paths[path].owned = 1;
		atomic_fetch_and(&g_guard_taken, ~(1u << path));
	}
out:
	ksceKernelUnlockFastMutex(&g_route_mutex);
	return ret;
}

int audio_route_guard_restore_mode(int path, AudioRouteSetModeFn set_mode)
{
	if (!g_route_initialized || path < 0 || path > 1 || !set_mode)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;
	ksceKernelLockFastMutex(&g_route_mutex);
	int ret = 0;
	if (!g_paths[path].owned)
		goto out;
	if (native_mode(path) != 2) {
		/* Preserve changes made outside the guarded AVConfig import, too. */
		g_paths[path].owned = 0;
		goto out;
	}
	ret = set_mode(path, g_paths[path].restore_mode);
	if (ret >= 0)
		g_paths[path].owned = 0;
out:
	g_guard_owned = 0;
	ksceKernelUnlockFastMutex(&g_route_mutex);
	return ret;
}

int audio_route_guard_read_primary(AudioRouteReadFn read, void *pcm, int frames)
{
	ksceKernelLockFastMutex(&g_route_mutex);
	/* Queue and drain under the same ownership lock as routing requests. A
	 * new reader must never receive a destination or drain from our session. */
	int ret = g_paths[0].owned ? read(0, pcm, frames) : 1;
	ksceKernelUnlockFastMutex(&g_route_mutex);
	return ret;
}

int audio_route_guard_path_owned(int path)
{
	ksceKernelLockFastMutex(&g_route_mutex);
	int owned = g_paths[path].owned;
	ksceKernelUnlockFastMutex(&g_route_mutex);
	return owned;
}

int audio_route_guard_path_taken(int path)
{
	return (atomic_load(&g_guard_taken) & (1u << path)) != 0;
}

int audio_route_guard_install(void)
{
	int expected = GUARD_IDLE;
	if (!atomic_compare_exchange_strong(&g_guard_id, &expected, GUARD_INSTALLING))
		return expected >= 0 ? 0 : SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
	uintptr_t address;
	int ret = firmware_audio_data(&address);
	if (ret < 0) {
		atomic_store(&g_guard_id, GUARD_IDLE);
		return ret;
	}
	/* Capture claims already bound the validated native data before this
	 * worker installed the hook. The routing mutex outlives the hook. */
	ksceKernelLockFastMutex(&g_route_mutex);
	g_native_data = address;
	ksceKernelUnlockFastMutex(&g_route_mutex);
	ret = taiHookFunctionImportForKernel(KERNEL_PID, &g_guard_ref,
		"SceAVConfig", 0x15D711C1, 0xAC9383D0, avconfig_set_monitor_mode);
	if (ret < 0) {
		atomic_store(&g_guard_id, GUARD_IDLE);
		return ret;
	}
	atomic_store(&g_guard_id, ret);
	return 0;
}

void audio_route_guard_set_owned(int owned)
{
	ksceKernelLockFastMutex(&g_route_mutex);
	g_guard_owned = owned && atomic_load(&g_guard_id) >= 0 &&
		g_paths[0].owned && g_paths[1].owned && !atomic_load(&g_guard_taken) &&
		g_native_data && native_mode(0) == 2 && native_mode(1) == 2;
	ksceKernelUnlockFastMutex(&g_route_mutex);
}

int audio_route_guard_prepare_unload(void)
{
	int expected = GUARD_IDLE;
	if (atomic_compare_exchange_strong(&g_guard_id, &expected, GUARD_CLOSING) || expected == GUARD_CLOSING)
		return 0;
	return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
}

void audio_route_guard_diagnostic(void)
{
#ifdef DIAGNOSTIC
	static unsigned int previous;
	unsigned int held = atomic_load(&g_guard_held);
	if (held != previous) {
		previous = held;
		diagnostic_record("held local-route resets", (int)held);
	}
#endif
}
