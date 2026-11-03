#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SCE_UDCD_STATUS_DRIVER_STARTED 1
#define SCE_UDCD_STATUS_ACTIVATED 0x200
static atomic_int uvc_thread_run, uvc_recover_pending;
static struct {
	int mtp, storage, state, audio_fail, video_fail, restart_fail;
	int storage_during_drain, stop_calls, cancel_calls, release_calls, restarts;
} mock;
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
	if (mock.storage_during_drain) mock.storage = 1;
	return mock.video_fail ? -72 : 0;
}
static int uvc_start(void)
{
	assert(uvc_thread_run && mock.mtp == 1 && mock.storage != 1);
	assert(mock.stop_calls && mock.cancel_calls && mock.release_calls);
	mock.restarts++;
	return mock.restart_fail ? -73 : 0;
}
static void diagnostic_record(const char *stage, int value) {(void)stage; (void)value;}

#include "usb-recovery.inc"

static void reset(void)
{
	memset(&mock, 0, sizeof(mock));
	mock.mtp = 1; mock.state = 0x220;
	uvc_thread_run = uvc_recover_pending = 1;
}
int main(void)
{
	for (int condition = 0; condition < 6; condition++) {
		reset();
		if (condition == 0) uvc_recover_pending = 0;
		if (condition == 1) uvc_thread_run = 0;
		if (condition == 2) mock.storage = 1;
		if (condition == 3) mock.mtp = 2;
		if (condition == 4) mock.state = 0x100;
		if (condition == 5) mock.state = -1;
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
	puts("USB recovery: external stop/MTP restoration, storage exclusion and drain/failure gates passed");
	return 0;
}
