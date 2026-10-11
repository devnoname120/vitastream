#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vita_test_kernel.h"
#include "firmware_profiles.h"

#define KERNEL_PID 0x10005
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_NO_RESIDENT 1
#define SCE_KERNEL_START_FAILED 2
#define LOG(...) (void)0
#define UVC_DRIVER_NAME "vitastream"
enum {PROFILE, HOOK, THREAD, EVENT, VIDEO, AUDIO, REGISTER, START};
static int fail_at, validated, hook, thread, event, video, audio, registered;
static int hook_calls, resources, hook_releases;
static SceUID g_udcd_config_hook_uid, uvc_thread_id, uvc_event_flag_id;
static int g_udcd_config_hook_ref, uvc_udcd_driver, uvc_driver_registered, uvc_thread_run;
static SceUdcdEndpoint endpoints[3];
static int result(int stage) { return fail_at == stage ? -77 : 0; }
static void diagnostic_reset(void) {}
static void diagnostic_record(const char *stage, int value) {(void)stage; (void)value;}
static int firmware_prepare(SceUID *id)
{
	if (result(PROFILE) < 0) return -77;
	validated = 1; *id = 123; return 0;
}
static int uvc_serialize_config_hook(void) {return 0;}
static int uvc_thread(void) {return 0;}
static SceUID taiHookFunctionOffsetForKernel(SceUID pid, int *ref, SceUID id,
	int segment, unsigned int offset, int thumb, int (*callback)(void))
{
	assert(validated && pid == KERNEL_PID && id == 123 && segment == 0);
	assert(offset == FIRMWARE_UDCD_CONFIG_OFFSET && thumb == 1);
	assert(ref == &g_udcd_config_hook_ref && callback == uvc_serialize_config_hook);
	hook_calls++;
	if (result(HOOK) < 0) return -77;
	hook = 1; return 22;
}
static int taiHookReleaseForKernel(SceUID id, int ref)
{
	(void)ref;
	assert(id == 22 && hook && !thread && !event && !video && !audio && !registered);
	hook = 0; hook_releases++; return 0;
}
static SceUID ksceKernelCreateThread(const char *name, int (*entry)(void),
	int priority, int stack_size, int flags, int affinity, int unused)
{
	(void)name; (void)entry; (void)priority; (void)stack_size;
	(void)flags; (void)affinity; (void)unused;
	assert(hook); resources++;
	if (result(THREAD) < 0) return -77;
	thread = 1; return 20;
}
SceUID ksceKernelCreateEventFlag(const char *name, unsigned int flags, unsigned int initial, void *opt)
{
	(void)name; (void)flags; (void)initial; (void)opt;
	assert(thread);
	if (result(EVENT) < 0) return -77;
	event = 1; return 21;
}
static int uvc_video_init(SceUdcdEndpoint *endpoint)
{
	assert(event && endpoint == &endpoints[1]);
	if (result(VIDEO) < 0) return -77;
	video = 1; return 0;
}
static int uac_audio_init(SceUdcdEndpoint *endpoint)
{
	assert(video && endpoint == &endpoints[2]);
	if (result(AUDIO) < 0) return -77;
	audio = 1; return 0;
}
static int ksceUdcdRegister(void *driver)
{
	assert(audio && driver == &uvc_udcd_driver);
	if (result(REGISTER) < 0) return -77;
	registered = 1; return 0;
}
static int ksceKernelStartThread(SceUID id, int argc, const void *args)
{
	(void)argc; (void)args;
	assert(id == 20 && registered && uvc_thread_run);
	return result(START);
}
static int ksceUdcdUnregister(void *driver)
{
	assert(driver == &uvc_udcd_driver && registered); registered = 0; return 0;
}
static int uac_audio_term(void) {assert(audio && !registered); audio = 0; return 0;}
static int uvc_video_term(void) {assert(video && !audio); video = 0; return 0;}
int ksceKernelDeleteEventFlag(SceUID id)
{
	assert(id == 21 && event && !video && !audio); event = 0; return 0;
}
static int ksceKernelDeleteThread(SceUID id)
{
	assert(id == 20 && thread && !event); thread = 0; return 0;
}

#include "startup.inc"

int main(void)
{
	for (fail_at = PROFILE; fail_at <= START; fail_at++) {
		validated = hook = thread = event = video = audio = registered = 0;
		hook_calls = resources = hook_releases = uvc_driver_registered = uvc_thread_run = 0;
		g_udcd_config_hook_uid = -1;
		assert(module_start(0, NULL) == (fail_at == PROFILE ? 1 : 2));
		assert(!hook && !thread && !event && !video && !audio && !registered);
		if (fail_at == PROFILE) assert(!hook_calls && !resources);
		if (fail_at == HOOK) assert(hook_calls == 1 && !resources && !hook_releases);
		if (fail_at > HOOK) assert(hook_releases == 1 && g_udcd_config_hook_uid == -1);
	}
	fail_at = -1;
	assert(module_start(0, NULL) == 0 && hook && registered);
	puts("startup: unsupported profile, hook and six resource failures clean up; supported startup succeeds");
	return 0;
}
