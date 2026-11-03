#include <assert.h>
#include <stdio.h>
#include <string.h>

#define UVC_DRIVER_NAME "vitastream_udcd_driver"
#define UVC_USB_PID 0x1337
#define SCE_UDCD_ERROR_INVALID_ARGUMENT (-2)
#define LOG(...) ((void)0)
static unsigned char uvc_probe_control_setting[16];
static const unsigned char uvc_probe_control_setting_default[16] = {1, 2, 3, 4};
static struct {
	int deactivate_result, fail_start, fail_activate, controller, mtp, uvc, pid;
	int start_calls, activate_calls, stop_uvc_calls;
} mock;
static void diagnostic_record(const char *stage, int value) {(void)stage; (void)value;}
static int ksceUdcdDeactivate(void) {mock.pid = 0; return mock.deactivate_result;}
static int ksceUdcdStop(const char *name, int size, void *args)
{
	assert(size == 0 && args == NULL);
	if (!strcmp(name, "USBDeviceControllerDriver")) mock.controller = 0;
	else if (!strcmp(name, "USB_MTP_Driver")) mock.mtp = 0;
	else if (!strcmp(name, UVC_DRIVER_NAME)) {mock.uvc = 0; mock.stop_uvc_calls++;}
	else assert(!strcmp(name, "USBPSPCommunicationDriver") || !strcmp(name, "USBSerDriver"));
	return 0;
}
static int ksceUdcdStart(const char *name, int size, void *args)
{
	assert(size == 0 && args == NULL); mock.start_calls++;
	if (mock.start_calls == mock.fail_start) return -71;
	if (!strcmp(name, "USBDeviceControllerDriver")) mock.controller = 1;
	else if (!strcmp(name, "USB_MTP_Driver")) {assert(mock.controller); mock.mtp = 1;}
	else {assert(!strcmp(name, UVC_DRIVER_NAME) && mock.controller); mock.uvc = 1;}
	return 0;
}
static int ksceUdcdActivate(int pid)
{
	mock.activate_calls++;
	if (mock.activate_calls == mock.fail_activate) return -72;
	assert(mock.controller && ((pid == 0x1337 && mock.uvc) || (pid == 0x4E4 && mock.mtp)));
	mock.pid = pid; return 0;
}

#include "usb-start.inc"

static void reset(void)
{
	memset(&mock, 0, sizeof(mock)); mock.controller = mock.mtp = 1; mock.pid = 0x4E4;
	memset(uvc_probe_control_setting, 255, sizeof(uvc_probe_control_setting));
}
int main(void)
{
	reset(); mock.deactivate_result = -70;
	assert(uvc_start() == -70 && !mock.start_calls);
	for (int failure = 0; failure < 3; failure++) {
		reset(); mock.fail_start = failure < 2 ? failure + 1 : 0;
		mock.fail_activate = failure == 2 ? 1 : 0;
		assert(uvc_start() == (failure < 2 ? -71 : -72));
		assert(mock.controller && mock.mtp && !mock.uvc && mock.pid == 0x4E4);
	}
	for (int deactivated = 0; deactivated < 2; deactivated++) {
		reset(); mock.deactivate_result = deactivated ? -2 : 0;
		assert(uvc_start() == 0 && mock.controller && mock.uvc && !mock.mtp && mock.pid == 0x1337);
		assert(mock.stop_uvc_calls == 1);
		assert(!memcmp(uvc_probe_control_setting, uvc_probe_control_setting_default, 16));
	}
	puts("USB start: restart sequencing, already-deactivated bus and MTP rollback after start/activation failures passed");
	return 0;
}
