#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/usb_state.h"
#include "../include/video_diagnostic.h"

#define UVC_DRIVER_NAME "vitastream"
#define SCE_EVENT_WAITOR 1u
#define SCE_EVENT_WAITCLEAR_PAT 2u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT ((int)0x80028005)
#define SCE_UDCD_ERROR_INVALID_ARGUMENT ((int)0x80243002)
#define SCE_UDCD_ERROR_DRIVER_IN_PROGRESS ((int)0x80243006)
typedef int32_t SceUID;
typedef uint32_t SceUInt32;
typedef uint32_t SceKernelIntrStatus;
typedef int (*SceSysEventHandler)(int, int, void *, void *);

static atomic_int uvc_thread_run, uvc_usb_yielded, uvc_recover_pending;
static atomic_int uvc_audio_wanted, uvc_audio_control_dirty;
static int uvc_event_flag_id = 42;
static struct {
	int uvc_state, storage_state;
	int waits, wakes, clears, aborts, cancel_calls, audio_requests, audio_stops;
	int releases, screen_restores, deactivates, stop_uvc, stop_controller, starts;
	int audio_error, release_errors, screen_error, deactivate_error;
	int stop_uvc_error, stop_controller_error, start_error, wait_error;
	int register_error, unregister_error, registrations, unregisters;
	int keep_uvc_started;
	SceKernelIntrStatus irq_mask;
	unsigned int event_bits;
	void (*on_wait)(void);
	void (*on_audio_stop)(void);
} mock;

static SceKernelIntrStatus ksceKernelCpuSuspendIntr(void) {return mock.irq_mask;}
static SceKernelIntrStatus ksceKernelCpuResumeIntr(SceKernelIntrStatus mask)
{
	assert(mask == mock.irq_mask);
	return mask;
}

static int ksceUdcdGetDrvState(const char *name)
{
	if (!strcmp(name, UVC_DRIVER_NAME)) return mock.uvc_state;
	assert(!strcmp(name, "USBStor_Driver"));
	return mock.storage_state;
}
static int ksceKernelClearEventFlag(int id, unsigned int mask)
{
	assert(id == uvc_event_flag_id);
	mock.clears++;
	mock.event_bits &= mask;
	return 0;
}
static int ksceKernelSetEventFlag(int id, unsigned int bits)
{
	assert(id == uvc_event_flag_id);
	mock.wakes++;
	mock.event_bits |= bits;
	return 0;
}
static int ksceKernelWaitEventFlag(int id, unsigned int bits, unsigned int mode,
	unsigned int *out, SceUInt32 *timeout)
{
	assert(id == uvc_event_flag_id && mode == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT));
	assert(timeout && *timeout == 4000000);
	mock.waits++;
	if (mock.on_wait) {
		void (*hook)(void) = mock.on_wait;
		mock.on_wait = NULL;
		hook();
	}
	if (mock.wait_error)
		return mock.wait_error;
	assert(mock.event_bits & bits);
	if (out) *out = mock.event_bits;
	mock.event_bits &= ~bits;
	return 0;
}
static void uvc_handle_video_abort(void) {mock.aborts++;}
static void uvc_video_cancel(void) {mock.cancel_calls++;}
static void uac_audio_request_stop(void) {mock.audio_requests++;}
static int uac_audio_stop_sync(void)
{
	mock.audio_stops++;
	if (mock.on_audio_stop) mock.on_audio_stop();
	return mock.audio_error;
}
static int uvc_video_release(void)
{
	mock.releases++;
	return mock.releases <= mock.release_errors ? -72 : 0;
}
static int vitastream_screen_restore(void)
{
	mock.screen_restores++;
	return mock.screen_error;
}
static int ksceUdcdDeactivate(void)
{
	mock.deactivates++;
	return mock.deactivate_error;
}
static int ksceUdcdStop(const char *name, int size, void *args)
{
	assert(size == 0 && args == NULL);
	if (!strcmp(name, UVC_DRIVER_NAME)) {
		mock.stop_uvc++;
		if (!mock.stop_uvc_error && !mock.keep_uvc_started)
			mock.uvc_state = VITASTREAM_USB_DRIVER_REGISTERED;
		return mock.stop_uvc_error;
	}
	assert(!strcmp(name, "USBDeviceControllerDriver"));
	mock.stop_controller++;
	return mock.stop_controller_error;
}
static int uvc_start(void)
{
	assert(atomic_load(&uvc_thread_run));
	assert(mock.storage_state != VITASTREAM_USB_DRIVER_STARTED);
	mock.starts++;
	if (!mock.start_error) mock.uvc_state = VITASTREAM_USB_DRIVER_STARTED;
	return mock.start_error;
}
static void diagnostic_record(const char *stage, int value)
{
	(void)stage; (void)value;
}
static SceSysEventHandler registered_handler;
static SceUID ksceKernelRegisterSysEventHandler(const char *name,
	SceSysEventHandler handler, void *args)
{
	assert(!strcmp(name, "vitastream") && handler && args == NULL);
	mock.registrations++;
	if (mock.register_error) return mock.register_error;
	registered_handler = handler;
	return 77;
}
static int ksceKernelUnregisterSysEventHandler(SceUID id)
{
	assert(id == 77 && registered_handler);
	mock.unregisters++;
	if (mock.unregister_error) return mock.unregister_error;
	registered_handler = NULL;
	return 0;
}

#include "suspend.inc"

static void run_worker(void)
{
	(void)uvc_suspend_worker();
}
static void expire_preparation(void)
{
	atomic_store(&uvc_suspend_state, UVC_SUSPEND_QUIESCED);
}
static void reset(void)
{
	memset(&mock, 0, sizeof(mock));
	mock.uvc_state = VITASTREAM_USB_DRIVER_STARTED;
	mock.storage_state = VITASTREAM_USB_DRIVER_REGISTERED;
	uvc_thread_run = 1;
	uvc_usb_yielded = uvc_recover_pending = 0;
	uvc_audio_wanted = 1;
	uvc_audio_control_dirty = 0;
	uvc_suspend_reset();
	atomic_store(&g_suspend_handler_ready, 0);
	g_suspend_handler_id = -1;
	registered_handler = NULL;
}
static void suspend_success(void)
{
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_SUSPEND_BEGIN, NULL, NULL) == 0);
	assert(!mock.waits && !mock.deactivates && !uvc_suspend_active());
	mock.on_wait = run_worker;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED);
	assert(uvc_suspend_active());
	assert(uvc_suspend_blocks_usb_start());
	assert(atomic_load(&uvc_suspend_owned) && atomic_load(&uvc_suspend_quiesce_done));
	assert(mock.waits == 1 && mock.aborts == 1 && mock.cancel_calls == 1);
	assert(mock.audio_requests == 1 && mock.audio_stops == 1 && mock.releases == 1);
	assert(mock.deactivates == 1 && mock.stop_uvc == 1 && mock.stop_controller == 1);
	assert(!mock.starts && !uvc_recover_pending);
}
int main(void)
{
	reset();
	assert(uvc_suspend_init() == 0 && mock.registrations == 1);
	assert(g_suspend_handler_id == 77 && !atomic_load(&g_suspend_handler_ready));
	assert(registered_handler == uvc_system_event_bridge);
	assert(uvc_system_event_bridge(0, UVC_SYS_EVENT_SUSPEND_BEGIN, NULL, NULL) == 0);
	assert(!mock.waits);
	atomic_store(&g_suspend_handler_ready, 1);
	assert(uvc_system_event_bridge(0, 0x200, NULL, NULL) == 0);
	assert(uvc_suspend_term() == 0 && mock.unregisters == 1);
	assert(g_suspend_handler_id == -1 && !atomic_load(&g_suspend_handler_ready));

	reset(); mock.register_error = -75;
	assert(uvc_suspend_init() == -75 && g_suspend_handler_id == -75);
	assert(uvc_suspend_term() == 0 && !mock.unregisters);

	reset(); assert(uvc_suspend_init() == 0);
	atomic_store(&g_suspend_handler_ready, 1);
	mock.unregister_error = -76;
	assert(uvc_suspend_term() == -76 && atomic_load(&g_suspend_handler_ready));
	mock.unregister_error = 0;
	assert(uvc_suspend_term() == 0 && !atomic_load(&g_suspend_handler_ready));

	reset();
	assert(uvc_suspend_system_event(0, 0x200, NULL, NULL) == 0);
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_USB, NULL, NULL) == 0);
	assert(!mock.waits && atomic_load(&uvc_suspend_state) == UVC_SUSPEND_IDLE);

	reset(); mock.uvc_state = VITASTREAM_USB_DRIVER_REGISTERED;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED && !mock.waits);
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_IDLE && !mock.starts);

	reset(); uvc_usb_yielded = 1;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	assert(!mock.waits && !mock.starts && uvc_usb_yielded);

	reset(); suspend_success();
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED);
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_RESUME);
	assert(uvc_suspend_worker() == 0 && mock.starts == 1);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_IDLE);
	assert(!uvc_suspend_active());
	assert(!uvc_suspend_blocks_usb_start());

	reset(); suspend_success();
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_SUSPEND_ABORT, NULL, NULL) == 0);
	assert(uvc_suspend_worker() == 0 && mock.starts == 1);

	reset(); suspend_success();
	uvc_usb_yielded = 1;
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	assert(uvc_suspend_worker() == 0 && !mock.starts && uvc_usb_yielded);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_IDLE);

	/* The vetoable early phase must do nothing even with IRQs masked. */
	reset(); mock.irq_mask = 0x80;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_SUSPEND_BEGIN, NULL, NULL) == 0);
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(!mock.waits && !mock.deactivates && !uvc_suspend_active());
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_SUSPEND_ABORT, NULL, NULL) == 0);
	assert(!mock.starts && uvc_suspend_worker() == 0);

	/* Timeout must allow native power to continue, and must not start late
	 * teardown or USB restoration during native suspend. */
	reset(); mock.wait_error = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED);
	assert(uvc_suspend_worker() == 1 && !mock.deactivates && !mock.starts);
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	assert(uvc_suspend_worker() == 0 && !mock.deactivates && mock.starts == 1);

	reset(); mock.on_wait = run_worker; mock.on_audio_stop = expire_preparation;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_result) == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	assert(!mock.deactivates && !mock.stop_controller && !mock.releases && !mock.starts);

	reset(); mock.audio_error = -71; mock.release_errors = 1; mock.on_wait = run_worker;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_result) == -71);
	assert(mock.releases == 2 && mock.deactivates == 1 && mock.stop_controller == 1);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED && !mock.starts);

	/* A successful Stop return does not excuse a still-started custom driver. */
	reset(); mock.keep_uvc_started = 1; mock.on_wait = run_worker;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_result) == SCE_UDCD_ERROR_DRIVER_IN_PROGRESS);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED && !mock.starts);

	reset(); mock.stop_controller_error = -73; mock.on_wait = run_worker;
	assert(uvc_suspend_system_event(0, UVC_SYS_EVENT_PREPARE_USB, NULL, NULL) == 0);
	assert(atomic_load(&uvc_suspend_result) == -73);
	assert(atomic_load(&uvc_suspend_state) == UVC_SUSPEND_QUIESCED && !mock.starts);
	mock.stop_controller_error = 0;
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	assert(uvc_suspend_worker() == 0 && mock.starts == 1);

	reset(); suspend_success();
	assert(uvc_suspend_system_event(1, UVC_SYS_EVENT_RESUME_FINAL, NULL, NULL) == 0);
	mock.storage_state = VITASTREAM_USB_DRIVER_STARTED;
	assert(uvc_suspend_worker() == 1 && !mock.starts);
	mock.storage_state = VITASTREAM_USB_DRIVER_REGISTERED;
	mock.start_error = -74;
	assert(uvc_suspend_worker() == 1 && mock.starts == 1);
	mock.start_error = 0;
	assert(uvc_suspend_worker() == 0 && mock.starts == 2);

	puts("suspend: IRQ-safe preparation, non-veto failures, late cancellation, abort and resume passed");
	return 0;
}
