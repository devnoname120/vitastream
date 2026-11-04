#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <psp2kern/udcd.h>
#include <psp2kern/kernel/threadmgr/fast_mutex.h>
#include <taihen.h>

static unsigned char native_data[0xBD24];
static struct {
	int installs, original_calls, setter_calls, read_calls;
	int fail_validate, fail_install, fail_setter, fail_init;
	int last_path, last_mode;
	int (*hook)(int, int);
} mock;

int ksceKernelInitializeFastMutex(SceKernelFastMutex *m, const char *n, unsigned int a, void *o)
{(void)n; (void)a; (void)o; return mock.fail_init ? -74 : pthread_mutex_init(m, NULL);}
int ksceKernelFinalizeFastMutex(SceKernelFastMutex *m) {return pthread_mutex_destroy(m);}
int ksceKernelLockFastMutex(SceKernelFastMutex *m) {return pthread_mutex_lock(m);}
int ksceKernelUnlockFastMutex(SceKernelFastMutex *m) {return pthread_mutex_unlock(m);}
int firmware_audio_data(uintptr_t *address)
{*address = (uintptr_t)native_data; return mock.fail_validate ? -72 : 0;}
static int set_mode(int path, int mode)
{
	mock.setter_calls++; mock.last_path = path; mock.last_mode = mode;
	if (mock.fail_setter) return -75;
	if (path == 0) native_data[0xBB28] = (unsigned char)mode;
	if (path == 1) native_data[0xB9BC] = (unsigned char)mode;
	return 0;
}
static int original(int path, int mode)
{mock.original_calls++; int ret = set_mode(path, mode); return ret < 0 ? ret : 37;}
static int read_pcm(int path, void *pcm, int frames)
{assert(path == 0 && !pcm && frames == 512); mock.read_calls++; return 0;}
static int read_buffer(int path, void *pcm, int frames)
{assert(path == 0 && pcm && frames == 512); mock.read_calls++; *(int *)pcm = 123; return 0;}
SceUID taiHookFunctionImportForKernel(SceUID pid, tai_hook_ref_t *ref,
	const char *module, uint32_t library, uint32_t function, const void *hook)
{
	assert(pid == KERNEL_PID && !strcmp(module, "SceAVConfig"));
	assert(library == 0x15D711C1 && function == 0xAC9383D0);
	mock.installs++; if (mock.fail_install) return -73;
	*ref = (uintptr_t)original; mock.hook = (int (*)(int, int))hook; return 31;
}
#include "../src/audio_route.c"

static void reset(void)
{
	if (g_route_initialized) assert(ksceKernelFinalizeFastMutex(&g_route_mutex) == 0);
	g_route_initialized = g_guard_owned = 0; g_native_data = 0;
	memset(g_paths, 0, sizeof(g_paths)); memset(&mock, 0, sizeof(mock));
	memset(native_data, 0, sizeof(native_data));
	atomic_store(&g_guard_id, GUARD_IDLE);
	atomic_store(&g_guard_taken, 0); atomic_store(&g_guard_held, 0);
	assert(audio_route_guard_init() == 0);
}
static void capture(int prior_secondary)
{
	native_data[0xB9BC] = (unsigned char)prior_secondary;
	assert(audio_route_guard_capture_mode(0, (uintptr_t)native_data, set_mode) == 0);
	assert(audio_route_guard_capture_mode(1, (uintptr_t)native_data, set_mode) == 0);
	assert(audio_route_guard_install() == 0);
	audio_route_guard_set_owned(1);
	assert(g_guard_owned);
}

/* A new request arriving during cleanup must execute after the old restore,
 * rather than being overwritten after it has returned success to its caller. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int restore_entered, restore_continue;
static atomic_int request_started;
static int gated_restore(int path, int mode)
{
	assert(pthread_mutex_lock(&gate) == 0);
	restore_entered = 1; pthread_cond_broadcast(&changed);
	while (!restore_continue) pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	return set_mode(path, mode);
}
static void *restore_thread(void *arg)
{(void)arg; assert(audio_route_guard_restore_mode(0, gated_restore) == 0); return NULL;}
static void *request_thread(void *arg)
{(void)arg; atomic_store(&request_started, 1); assert(mock.hook(0, 1) == 37); return NULL;}
static int gated_read(int path, void *pcm, int frames)
{
	pthread_mutex_lock(&gate); restore_entered = 1; pthread_cond_broadcast(&changed);
	while (!restore_continue) pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	return read_pcm(path, pcm, frames);
}
static void *drain_thread(void *arg)
{(void)arg; assert(audio_route_guard_read_primary(gated_read, NULL, 512) == 0); return NULL;}

int main(void)
{
	reset(); audio_route_guard_term(); mock.fail_init = 1;
	assert(audio_route_guard_init() == -74 && !g_route_initialized);
	reset(); mock.fail_validate = 1;
	assert(audio_route_guard_install() == -72 && !mock.installs);
	assert(audio_route_guard_prepare_unload() == 0);
	reset(); mock.fail_install = 1;
	assert(audio_route_guard_install() == -73);
	assert(audio_route_guard_prepare_unload() == 0);
	reset(); assert(audio_route_guard_prepare_unload() == 0);
	assert(audio_route_guard_install() < 0 && !mock.installs);
	reset(); atomic_store(&g_guard_id, GUARD_INSTALLING);
	assert(audio_route_guard_prepare_unload() < 0);

	/* Snapshot fallback, with no policy request while capture is active. */
	reset(); capture(-2);
	assert(audio_route_guard_restore_mode(0, set_mode) == 0);
	assert(audio_route_guard_restore_mode(1, set_mode) == 0);
	assert(native_data[0xBB28] == 0 && native_data[0xB9BC] == 0xFE);

	/* A blocked default request replaces the old -2 snapshot with mode 0. */
	reset(); capture(-2); int calls = mock.setter_calls;
	assert(mock.hook(0, 0) == 0 && mock.hook(1, 0) == 0);
	assert(mock.setter_calls == calls && atomic_load(&g_guard_held) == 2);
	assert(native_data[0xBB28] == 2 && native_data[0xB9BC] == 2);
	assert(audio_route_guard_restore_mode(0, set_mode) == 0);
	assert(audio_route_guard_restore_mode(1, set_mode) == 0);
	assert(native_data[0xBB28] == 0 && native_data[0xB9BC] == 0);

	/* Recovery preserves the newer target instead of snapshotting our override. */
	reset(); capture(-2); assert(mock.hook(1, 0) == 0);
	native_data[0xBB28] = native_data[0xB9BC] = 0;
	assert(audio_route_guard_capture_mode(0, (uintptr_t)native_data, set_mode) == 0);
	assert(audio_route_guard_capture_mode(1, (uintptr_t)native_data, set_mode) == 0);
	assert(audio_route_guard_restore_mode(1, set_mode) == 0 && native_data[0xB9BC] == 0);

	/* New owners on either path supersede a previously blocked default,
	 * including a new owner requesting the same mode 2 as our override. */
	for (int path = 0; path < 2; path++) for (int mode = 1; mode <= 2; mode++) {
		reset(); capture(-2); assert(mock.hook(path, 0) == 0);
		assert(mock.hook(path, mode) == 37 && !g_guard_owned);
		assert(audio_route_guard_path_taken(path) && !audio_route_guard_path_owned(path));
		calls = mock.setter_calls;
		assert(audio_route_guard_restore_mode(path, set_mode) == 0 && mock.setter_calls == calls);
		assert(native_mode(path) == mode);
		if (path == 0) assert(audio_route_guard_read_primary(read_pcm, NULL, 512) == 1 && !mock.read_calls);
	}
	reset(); capture(-2); int sample = 0;
	assert(audio_route_guard_read_primary(read_buffer, &sample, 512) == 0 && sample == 123);
	assert(mock.hook(0, 2) == 37); sample = 0;
	assert(audio_route_guard_read_primary(read_buffer, &sample, 512) == 1);
	assert(sample == 0 && mock.read_calls == 1);
	/* A later ordinary request also reaches the owner after we relinquished. */
	reset(); capture(-2); assert(mock.hook(0, 0) == 0);
	assert(mock.hook(0, 1) == 37 && mock.hook(0, 0) == 37);
	calls = mock.setter_calls;
	assert(audio_route_guard_restore_mode(0, set_mode) == 0 && mock.setter_calls == calls);

	/* A failed alternative request did not establish a new owner. */
	reset(); capture(-2); assert(mock.hook(1, 0) == 0);
	mock.fail_setter = 1; assert(mock.hook(1, 1) == -75);
	assert(!audio_route_guard_path_taken(1) && audio_route_guard_path_owned(1) && g_guard_owned);
	mock.fail_setter = 0; assert(audio_route_guard_restore_mode(1, set_mode) == 0);
	assert(native_data[0xB9BC] == 0);
	/* A failed forwarded default request is not a successful desired update. */
	reset(); capture(-2); audio_route_guard_set_owned(0); mock.fail_setter = 1;
	assert(mock.hook(1, 0) == -75);
	mock.fail_setter = 0; assert(audio_route_guard_restore_mode(1, set_mode) == 0);
	assert(native_data[0xB9BC] == 0xFE);
	/* Keep the claim when restoration fails, allowing a safe retry. */
	reset(); capture(-2); mock.fail_setter = 1;
	assert(audio_route_guard_restore_mode(1, set_mode) == -75 && audio_route_guard_path_owned(1));
	mock.fail_setter = 0; assert(audio_route_guard_restore_mode(1, set_mode) == 0);
	assert(native_data[0xB9BC] == 0xFE);
	/* Unobserved direct changes to a different native mode are preserved. */
	reset(); capture(-2); native_data[0xBB28] = 1; calls = mock.setter_calls;
	assert(audio_route_guard_restore_mode(0, set_mode) == 0 && mock.setter_calls == calls);

	reset(); capture(-2); assert(mock.hook(0, 0) == 0);
	pthread_t restore_worker, request_worker; restore_entered = restore_continue = 0;
	atomic_store(&request_started, 0);
	assert(pthread_create(&restore_worker, NULL, restore_thread, NULL) == 0);
	pthread_mutex_lock(&gate);
	while (!restore_entered) pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	assert(pthread_create(&request_worker, NULL, request_thread, NULL) == 0);
	while (!atomic_load(&request_started)) sched_yield();
	pthread_mutex_lock(&gate); restore_continue = 1; pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	pthread_join(restore_worker, NULL); pthread_join(request_worker, NULL);
	assert(native_data[0xBB28] == 1 && !audio_route_guard_path_owned(0));
	/* The same serialization protects the NULL drain: a new reader cannot
	 * acquire the route and then receive our old capture's drain request. */
	reset(); capture(-2); restore_entered = restore_continue = 0;
	atomic_store(&request_started, 0);
	assert(pthread_create(&restore_worker, NULL, drain_thread, NULL) == 0);
	pthread_mutex_lock(&gate);
	while (!restore_entered) pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	assert(pthread_create(&request_worker, NULL, request_thread, NULL) == 0);
	while (!atomic_load(&request_started)) sched_yield();
	pthread_mutex_lock(&gate); restore_continue = 1; pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	pthread_join(restore_worker, NULL); pthread_join(request_worker, NULL);
	assert(mock.read_calls == 1 && native_data[0xBB28] == 1);
	calls = mock.setter_calls;
	assert(audio_route_guard_restore_mode(0, set_mode) == 0 && mock.setter_calls == calls);
	assert(audio_route_guard_prepare_unload() < 0);
	/* Unrelated paths remain transparent. */
	assert(mock.hook(3, 0) == 37 && mock.last_path == 3);
	puts("audio route: latest restore target, both ownership paths, failed requests, recovery and concurrent cleanup passed");
	return 0;
}
