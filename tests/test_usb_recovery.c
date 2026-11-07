#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/usb_state.h"
static atomic_int uvc_thread_run, uvc_recover_pending, uvc_usb_yielded;
static atomic_int uvc_usb_yield_complete, uvc_usb_resume_pending, suspend_active;
static int uvc_suspend_active(void) { return atomic_load(&suspend_active); }
static int g_usb_handoff_mutex, uvc_event_flag_id = 42;
enum { UVC_EVENT_USB_YIELDED = 2 };
static uint64_t mock_now;
static struct {
	int mtp, storage, state, audio_fail, video_fail, restart_fail;
	int storage_during_drain, stop_calls, cancel_calls, release_calls, restarts;
	int attach_during_start;
	unsigned int event_bits;
} mock;
static int ksceKernelTryLockFastMutex(int *mutex)
{
	assert(mutex == &g_usb_handoff_mutex);
	if (*mutex) return -1;
	*mutex = 1;
	return 0;
}
static int ksceKernelUnlockFastMutex(int *mutex)
{
	assert(mutex == &g_usb_handoff_mutex && *mutex);
	*mutex = 0;
	return 0;
}
static int ksceKernelClearEventFlag(int id, unsigned int mask)
{
	assert(id == uvc_event_flag_id);
	mock.event_bits &= mask;
	return 0;
}
static uint64_t ksceKernelGetSystemTimeWide(void) {return mock_now;}
static int ksceUdcdGetDrvState(const char *name)
{
	if (!strcmp(name, "USB_MTP_Driver")) return mock.mtp;
	assert(!strcmp(name, "USBStor_Driver"));
	return mock.storage;
}
static int ksceUdcdGetDeviceState(void) {return mock.state;}
static int uac_audio_stop_sync(void)
{
	mock.stop_calls++;
	return mock.audio_fail ? -71 : 0;
}
static void uvc_video_cancel(void) {mock.cancel_calls++;}
static int uvc_video_release(void)
{
	mock.release_calls++;
	if (mock.storage_during_drain) mock.storage = VITASTREAM_USB_DRIVER_STARTED;
	return mock.video_fail ? -72 : 0;
}
static int uvc_start(void)
{
	assert(uvc_thread_run && !uvc_usb_yielded &&
		mock.storage != VITASTREAM_USB_DRIVER_STARTED);
	assert(!(mock.state & VITASTREAM_USB_ACTIVATED) ||
		mock.mtp == VITASTREAM_USB_DRIVER_STARTED);
	if (mock.attach_during_start) uvc_recover_pending = 0;
	mock.restarts++;
	return mock.restart_fail ? -73 : 0;
}
static void diagnostic_record(const char *stage, int value) {(void)stage; (void)value;}

#include "usb-recovery.inc"

static void reset(void)
{
	memset(&mock, 0, sizeof(mock));
	mock.mtp = VITASTREAM_USB_DRIVER_STARTED; mock.state = 0x2020;
	uvc_thread_run = uvc_recover_pending = 1;
	uvc_usb_yielded = uvc_usb_yield_complete = uvc_usb_resume_pending = suspend_active = 0;
	g_usb_handoff_mutex = 0;
	mock_now += 10000000;
}
static void external_resume(void)
{
	reset();
	mock.storage = VITASTREAM_USB_DRIVER_REGISTERED;
	mock.event_bits = UVC_EVENT_USB_YIELDED;
	uvc_usb_yielded = uvc_usb_yield_complete = uvc_usb_resume_pending = 1;
}
int main(void)
{
	for (int condition = 0; condition < 7; condition++) {
		reset();
		if (condition == 0) uvc_recover_pending = 0;
		if (condition == 1) uvc_thread_run = 0;
		if (condition == 2) mock.storage = VITASTREAM_USB_DRIVER_STARTED;
		if (condition == 3) mock.mtp = VITASTREAM_USB_DRIVER_REGISTERED;
		if (condition == 4) mock.state = 0x200;
		if (condition == 5) mock.state = -1;
		if (condition == 6) uvc_usb_yielded = 1;
		assert(uvc_recover_usb() == 0 && !mock.stop_calls && !mock.restarts);
	}
	reset(); mock.audio_fail = 1;
	assert(uvc_recover_usb() == -71 && !mock.cancel_calls && !mock.restarts && uvc_recover_pending);
	reset(); mock.video_fail = 1;
	assert(uvc_recover_usb() == -72 && mock.cancel_calls == 1 && !mock.restarts && uvc_recover_pending);
	reset(); mock.storage_during_drain = 1;
	assert(uvc_recover_usb() == 0 && mock.release_calls == 1 && !mock.restarts && uvc_recover_pending);
	reset(); mock.restart_fail = 1;
	assert(uvc_recover_usb() == -73 && mock.restarts == 1 && uvc_recover_pending);
	reset();
	assert(uvc_recover_usb() == 0 && mock.restarts == 1 && !uvc_recover_pending);
	assert(uvc_recover_usb() == 0 && mock.restarts == 1);
	/* Neither unplugging nor a queued resume may displace active storage. */
	for (int plugged = 0; plugged < 2; plugged++) {
		external_resume(); mock.state = plugged ? 0x2020 : 0;
		mock.storage = VITASTREAM_USB_DRIVER_STARTED;
		assert(uvc_resume_usb() == 0 && !mock.restarts && uvc_usb_resume_pending);
		mock.storage = VITASTREAM_USB_DRIVER_REGISTERED;
		mock.state = 0x2000; /* Activated MTP without CONNECTED. */
		assert(uvc_resume_usb() == 0 && mock.restarts == 1);
		assert(!uvc_usb_yielded && !uvc_usb_resume_pending && !mock.event_bits);
	}
	/* A failed MTP activation on unplug still leaves an idle bus to arm. */
	external_resume(); mock.mtp = VITASTREAM_USB_DRIVER_REGISTERED; mock.state = 0;
	assert(uvc_resume_usb() == 0 && mock.restarts == 1 && !uvc_usb_yielded);
	external_resume(); mock.restart_fail = mock.attach_during_start = 1;
	assert(uvc_resume_usb() == -73 && mock.restarts == 1 && !uvc_recover_pending);
	assert(uvc_usb_yielded && !uvc_usb_yield_complete && uvc_usb_resume_pending);
	mock.restart_fail = 0;
	uvc_usb_yield_complete = 1; /* Next worker drain has completed. */
	assert(uvc_resume_usb() == 0 && mock.restarts == 1 && uvc_usb_resume_pending);
	mock_now += 1000000;
	assert(uvc_resume_usb() == 0 && mock.restarts == 2 && !uvc_usb_resume_pending);
	external_resume(); mock.mtp = VITASTREAM_USB_DRIVER_REGISTERED;
	assert(uvc_resume_usb() == 0 && !mock.restarts && uvc_usb_resume_pending);
	mock.state = 0;
	assert(uvc_resume_usb() == 0 && mock.restarts == 1 && !uvc_usb_resume_pending);
	external_resume(); g_usb_handoff_mutex = 1;
	assert(uvc_resume_usb() == 0 && !mock.restarts && uvc_usb_resume_pending);
	assert(mock.event_bits == UVC_EVENT_USB_YIELDED);
	g_usb_handoff_mutex = 0;
	assert(uvc_resume_usb() == 0 && mock.restarts == 1);
	for (int condition = 0; condition < 6; condition++) {
		external_resume();
		if (condition == 0) uvc_usb_resume_pending = 0;
		if (condition == 1) uvc_usb_yielded = 0;
		if (condition == 2) uvc_usb_yield_complete = 0;
		if (condition == 3) uvc_thread_run = 0;
		if (condition == 4) mock.storage = 3;
		if (condition == 5) mock.state = -1;
		assert(uvc_resume_usb() == 0 && !mock.restarts);
	}
	puts("USB recovery: storage exclusion, disconnected resume, retries and handoff gates passed");
	return 0;
}
