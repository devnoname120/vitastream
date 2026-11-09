#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/fast_mutex.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysmem/uid_guid.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/udcd.h>
#include <psp2kern/display.h>
#include <psp2kern/lowio/iftu.h>
#include <taihen.h>
#include <string.h>
#include <stdatomic.h>
#include "audio.h"
#include "diagnostic.h"
#include "firmware.h"
#include "firmware_profiles.h"
#include "usb_descriptors.h"
#include "uvc.h"
#include "video.h"
#include "settings.h"
#include "usb_state.h"

#ifdef DEBUG

#include "log.h"
#include "draw.h"
#include "console.h"

#define LOG(s, ...) \
	do { \
		char __buffer[128]; \
		snprintf(__buffer, sizeof(__buffer), s, ##__VA_ARGS__); \
		/*LOG_TO_FILE(__buffer);*/ \
		console_print(__buffer); \
	} while (0)
#else
#define LOG(...) (void)0
#endif

#define ALIGN(x, a)			(((x) + ((a) - 1)) & ~((a) - 1))
#define UNUSED(x)			((void)x)

#define UVC_DRIVER_NAME			"vitastream"
#define UVC_USB_PID			0x1337

#define SCE_MTP_IF_FOR_DRIVER_NID		0x73309DFE
#define SCE_MTP_IF_STOP_DRIVER_NID	0xFDF8C3C0
#define SCE_MTP_IF_START_DRIVER_NID	0xCB685AC0

#define SCE_DISPLAY_PIXELFORMAT_BGRA5551 0x50000000

/* Exported by the pinned SDK, but its older sysmem header omits this ABI. */
int ksceKernelProcModeVAtoPA(SceUID pid, SceUInt32 permission,
	const void *va, void **pa);

/* Compatibility copy of the corrected SceIftuPlaneState definition. */
typedef struct SceIftuPlaneState_updated {
	SceIftuFrameBuf fb;
	unsigned int reserved[3];	/* Unused in FW 3.60 */
	unsigned int src_w;		/* Inverse scaling factor in 16.16 fixed point. */
	unsigned int src_h;		/* Inverse scaling factor in 16.16 fixed point. */
	unsigned int dst_x;		/* Offset into the destination buffer. */
	unsigned int dst_y;		/* Offset into the destination buffer. */
	unsigned int src_x;		/* Offset into the source buffer in 8.8 fixed point. */
	unsigned int src_y;		/* Offset into the source buffer in 8.8 fixed point. */
	unsigned int crop_top;
	unsigned int crop_bottom;
	unsigned int crop_left;
	unsigned int crop_right;
} SceIftuPlaneState_updated;
_Static_assert(sizeof(SceIftuPlaneState_updated) == 0x54,
	"unexpected SceIftuPlaneState_updated size");

static const struct uvc_streaming_control uvc_probe_control_setting_default = {
	.bmHint				= 0,
	.bFormatIndex			= FORMAT_INDEX_UNCOMPRESSED_NV12,
	.bFrameIndex			= 1,
	.dwFrameInterval		= FPS_TO_INTERVAL(60),
	.wKeyFrameRate			= 0,
	.wPFrameRate			= 0,
	.wCompQuality			= 0,
	.wCompWindowSize		= 0,
	.wDelay				= 0,
	.dwMaxVideoFrameSize		= MAX_UVC_VIDEO_FRAME_SIZE,
	.dwMaxPayloadTransferSize	= MAX_UVC_PAYLOAD_TRANSFER_SIZE,
	.dwClockFrequency		= 0,
	.bmFramingInfo			= 0,
	.bPreferedVersion		= 1,
	.bMinVersion			= 0,
	.bMaxVersion			= 0,
};

static struct uvc_streaming_control uvc_probe_control_setting;

static struct {
	unsigned char buffer[64];
	SceUdcdEP0DeviceRequest ep0_req;
} pending_recv;

static SceUID uvc_thread_id;
static SceUID uvc_event_flag_id;
static atomic_int uvc_thread_run;
static atomic_int stream;
static atomic_uint uvc_capture_interval = 166666;
static atomic_uint uvc_pacing_epoch;
static atomic_int uvc_recover_pending;
static int uvc_driver_registered;
static atomic_int uvc_audio_wanted, uvc_audio_control_dirty, uvc_audio_attach_pending;
static atomic_int uvc_usb_yielded, uvc_usb_yield_complete, uvc_usb_resume_pending;
enum { MTP_HOOK_IDLE = -1, MTP_HOOK_INSTALLING = -2, MTP_HOOK_CLOSING = -3 };
static atomic_int g_mtp_stop_hook_uid = ATOMIC_VAR_INIT(MTP_HOOK_IDLE);
static tai_hook_ref_t g_mtp_stop_hook_ref;
static atomic_int g_mtp_start_hook_uid = ATOMIC_VAR_INIT(MTP_HOOK_IDLE);
static tai_hook_ref_t g_mtp_start_hook_ref;
static SceKernelFastMutex g_usb_handoff_mutex;
static int g_usb_handoff_mutex_initialized;
enum { UVC_EVENT_USB_YIELDED = 2 };


static int usb_ep0_req_send(const void *data, unsigned int size)
{
	static SceUdcdDeviceRequest req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)data,
		.attributes = 0,
		.size = size,
		.isControlRequest = 0,
		.onComplete = NULL,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	return ksceUdcdReqSend(&req);
}

static void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req);

static int usb_ep0_enqueue_recv_for_req(const SceUdcdEP0DeviceRequest *ep0_req)
{
	static SceUdcdDeviceRequest req;

	pending_recv.ep0_req = *ep0_req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)pending_recv.buffer,
		.attributes = 0,
		.size = pending_recv.ep0_req.wLength,
		.isControlRequest = 0,
		.onComplete = &usb_ep0_req_recv_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelDcacheInvalidateRange(pending_recv.buffer,
		pending_recv.ep0_req.wLength);

	return ksceUdcdReqRecv(&req);
}

static void uvc_handle_video_streaming_req_recv(const SceUdcdEP0DeviceRequest *req)
{
	if (uvc_usb_yielded)
		return;
	struct uvc_streaming_control *streaming_control =
		(struct uvc_streaming_control *)pending_recv.buffer;

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			LOG("Probe SET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			LOG("Commit SET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);

			atomic_store_explicit(&uvc_capture_interval,
				streaming_control->dwFrameInterval, memory_order_relaxed);
			atomic_fetch_add_explicit(&uvc_pacing_epoch, 1, memory_order_release);
			stream = 1;
			ksceKernelSetEventFlag(uvc_event_flag_id, 1);
			break;
		}
		break;
	}
}

void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req)
{
	switch (pending_recv.ep0_req.wIndex & 0xFF) {
	case STREAM_INTERFACE:
		uvc_handle_video_streaming_req_recv(&pending_recv.ep0_req);
		break;
	}
}

static void uvc_handle_interface_ctrl_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_interface_ctrl_req\n");
}

static void uvc_handle_input_terminal_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_input_terminal_req %x, %x\n", req->wValue, req->bRequest);
}

static void uvc_handle_output_terminal_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_output_terminal_req\n");
}

static void uvc_handle_video_streaming_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_video_streaming_req %x, %x\n", req->wValue, req->bRequest);

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_MIN:
		case UVC_GET_MAX:
		case UVC_GET_DEF:
			LOG("Probe GET_DEF, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting_default.bFormatIndex,
			    uvc_probe_control_setting_default.bmFramingInfo);
			usb_ep0_req_send(&uvc_probe_control_setting_default,
					 sizeof(uvc_probe_control_setting_default));
			break;
		case UVC_GET_CUR:
			LOG("Probe GET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);
			ksceKernelDcacheCleanRange(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_CUR:
			ksceKernelDcacheCleanRange(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	}
}

static void uvc_handle_video_abort(void)
{
	LOG("uvc_handle_video_abort\n");

	if (stream) {
		stream = 0;

		ksceUdcdClearFIFO(&endpoints[1]);
		ksceUdcdReqCancelAll(&endpoints[1]);
	}
}

static void uvc_handle_set_interface(const SceUdcdEP0DeviceRequest *req)
{
	LOG("uvc_handle_set_interface %x %x\n", req->wIndex, req->wValue);

	/* MAC OS sends Set Interface Alternate Setting 0 command after
	 * stopping to stream. This application needs to stop streaming. */
	if ((req->wIndex == STREAM_INTERFACE) && (req->wValue == 0))
		uvc_handle_video_abort();
}

static void uvc_handle_clear_feature(const SceUdcdEP0DeviceRequest *req)
{
	LOG("uvc_handle_clear_feature\n");

	/* Windows OS sends Clear Feature Request after it stops streaming,
	 * however MAC OS sends clear feature request right after it sends a
	 * Commit -> SET_CUR request. Hence, stop streaming only of streaming
	 * has started. */
	switch (req->wValue) {
	case USB_FEATURE_ENDPOINT_HALT:
		if ((req->wIndex & USB_ENDPOINT_ADDRESS_MASK) ==
		    endpoints[1].endpointNumber) {
			uvc_handle_video_abort();
		}
		break;
	}
}

static int uvc_udcd_process_request(int recipient, int arg, SceUdcdEP0DeviceRequest *req, void *user_data)
{
	LOG("usb_driver_process_request(recipient: %x, arg: %x)\n", recipient, arg);
	LOG("  request: %x type: %x wValue: %x wIndex: %x wLength: %x\n",
		req->bRequest, req->bmRequestType, req->wValue, req->wIndex, req->wLength);

	if (arg < 0)
		return -1;

	switch (req->bmRequestType) {
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0xA1 */
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x21 */
		switch (req->wIndex & 0xFF) {
		case CONTROL_INTERFACE:
			switch (req->wIndex >> 8) {
			case INTERFACE_CTRL_ID:
				uvc_handle_interface_ctrl_req(req);
				break;
			case INPUT_TERMINAL_ID:
				uvc_handle_input_terminal_req(req);
				break;
			case OUTPUT_TERMINAL_ID:
				uvc_handle_output_terminal_req(req);
				break;
			}
			break;
		case STREAM_INTERFACE:
			uvc_handle_video_streaming_req(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x01 */
		switch (req->bRequest) {
		case USB_REQ_SET_INTERFACE:
			uvc_handle_set_interface(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_ENDPOINT: /* 0x02 */
		switch (req->bRequest) {
		case USB_REQ_CLEAR_FEATURE:
			uvc_handle_clear_feature(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_DEVICE: /* 0x80 */
		switch (req->wValue >> 8) {
		case 0x0A: /* USB_DT_DEBUG */
			break;
		}
		break;
	default:
		LOG("Unknown bmRequestType: 0x%02X\n", req->bmRequestType);
	}

	return 0;
}

static int uvc_udcd_change_setting(int interfaceNumber, int alternateSetting, int bus)
{
	LOG("uvc_udcd_change %d %d\n", interfaceNumber, alternateSetting);

	if (interfaceNumber == AUDIO_STREAM_INTERFACE) {
		if (alternateSetting == 1 && uvc_usb_yielded)
			return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
		/* Native USB callbacks can run where waiting is forbidden. The video
		 * worker owns all blocking audio route/start/attach operations. */
		uvc_audio_wanted = alternateSetting == 1;
		uvc_audio_control_dirty = 1;
		if (alternateSetting != 1)
			uac_audio_request_stop();
		if (uvc_thread_run)
			ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	}

	return 0;
}

static int uvc_udcd_attach(int usb_version, void *user_data)
{
	LOG("uvc_udcd_attach %d\n", usb_version);

	ksceUdcdClearFIFO(&endpoints[1]);
	uvc_audio_wanted = 0;
	uvc_audio_attach_pending = 1;
	uvc_audio_control_dirty = 1;
	uac_audio_request_stop();
	if (uvc_thread_run)
		ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	uvc_recover_pending = 0;

	return 0;
}

static void uvc_udcd_detach(void *user_data)
{
	LOG("uvc_udcd_detach\n");

	uvc_handle_video_abort();
	uvc_audio_wanted = 0;
	uvc_audio_control_dirty = 1;
	uac_audio_request_stop();
	if (uvc_thread_run) {
		uvc_recover_pending = 1;
		ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	}

}

static void uvc_udcd_configure(int usb_version, int desc_count, SceUdcdInterfaceSettings *settings, void *user_data)
{
	LOG("uvc_udcd_configure %d %d %p %d\n", usb_version, desc_count, settings, settings->numDescriptors);
}

static int uvc_driver_start(int size, void *p, void *user_data)
{
	LOG("uvc_driver_start\n");

	return uvc_usb_yielded ? SCE_UDCD_ERROR_DRIVER_IN_PROGRESS : 0;
}

static int uvc_driver_stop(int size, void *p, void *user_data)
{
	LOG("uvc_driver_stop\n");

	uvc_handle_video_abort();
	uvc_audio_wanted = 0;
	uvc_audio_control_dirty = 1;
	uac_audio_request_stop();
	if (uvc_thread_run) {
		uvc_recover_pending = 1;
		ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	}
	return 0;
}

static SceUdcdDriver uvc_udcd_driver = {
	.driverName			= UVC_DRIVER_NAME,
	.numEndpoints			= 3,
	.endpoints			= endpoints,
	.interface			= &interface,
	.descriptor_hi			= &devdesc_hi,
	.configuration_hi		= &config_hi,
	.descriptor			= &devdesc_full,
	.configuration			= &config_full,
	.stringDescriptors		= NULL,
	.stringDescriptorProduct	= &string_descriptor_product,
	.stringDescriptorSerial		= &string_descriptor_serial,
	.processRequest			= &uvc_udcd_process_request,
	.changeSetting			= &uvc_udcd_change_setting,
	.attach				= &uvc_udcd_attach,
	.detach				= &uvc_udcd_detach,
	.configure			= &uvc_udcd_configure,
	.start				= &uvc_driver_start,
	.stop				= &uvc_driver_stop,
	.user_data			= NULL
};

int uvc_start(void);
int uvc_stop(void);

static void uvc_apply_audio_control(void)
{
	int attached = atomic_exchange(&uvc_audio_attach_pending, 0);
	if (!atomic_exchange(&uvc_audio_control_dirty, 0) && !attached)
		return;
	if (attached)
		uac_audio_on_attach();
	if (!uvc_audio_wanted || uvc_usb_yielded) {
		uac_audio_request_stop();
		return;
	}
	int state = ksceUdcdGetDeviceState();
	if (ksceUdcdGetDrvState(UVC_DRIVER_NAME) != VITASTREAM_USB_DRIVER_STARTED ||
	    state < 0 || !(state & VITASTREAM_USB_ACTIVATED) ||
	    !(state & VITASTREAM_USB_CONNECTED)) {
		/* Resume may report the alternate setting before the bus is ready. */
		uvc_audio_control_dirty = 1;
		return;
	}
	int ret = uac_audio_start();
	diagnostic_record("audio start on USB worker", ret);
	if (!uvc_audio_wanted || uvc_usb_yielded)
		uac_audio_request_stop();
}

static int uvc_complete_usb_handoff(void)
{
	if (uvc_usb_yield_complete)
		return 0;
	uvc_audio_wanted = 0;
	uac_audio_request_stop();
	uvc_video_cancel();
	int ret = uac_audio_stop_sync();
	if (ret < 0)
		return ret;
	/* The capture thread owns the optional Camera mutex. Only this thread
	 * may return it, after every outstanding transfer has completed. */
	ret = uvc_video_release();
	if (ret < 0)
		return ret;
	ret = vitastream_screen_restore();
	if (ret < 0)
		return ret;
	ret = ksceUdcdDeactivate();
	if (ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT)
		return ret;
	if (ksceUdcdGetDrvState(UVC_DRIVER_NAME) == VITASTREAM_USB_DRIVER_STARTED) {
		ret = ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
		if (ret < 0)
			return ret;
		ret = ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
		if (ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT)
			return ret;
	}
	uvc_usb_yield_complete = 1;
	ksceKernelSetEventFlag(uvc_event_flag_id, UVC_EVENT_USB_YIELDED);
	diagnostic_record("USB yielded to external owner", 0);
	return 0;
}

static int uvc_vitashell_usb_request(void)
{
	/* MtpIf is also used by SceShell's CMA connection manager. Its syscall
	 * wrapper calls the driver exports synchronously in the
	 * requesting process, so only VitaShell's USB lifecycle may yield UVC. */
	char title_id[16] = {0};
	SceUID pid = ksceKernelGetProcessId();
	return pid > 0 && pid != KERNEL_PID &&
		ksceKernelSysrootGetProcessTitleId(pid, title_id, sizeof(title_id)) >= 0 &&
		memcmp(title_id, "VITASHELL", sizeof("VITASHELL")) == 0;
}

static int mtp_stop_for_external_owner(int kind)
{
	/* Hook the storage-facing MtpIf wrapper, not the shared low-level MTP
	 * stop used during application/power transitions. */
	if (kind != 1 || !uvc_thread_run || !uvc_vitashell_usb_request())
		return TAI_CONTINUE(int, g_mtp_stop_hook_ref, kind);
	ksceKernelLockFastMutex(&g_usb_handoff_mutex);
	int ret;
	/* Also honour requests arriving just before uvc_start: the worker's
	 * yielded gates must prevent a late activation after storage takes over. */
	uvc_usb_resume_pending = 0;
	if (!uvc_usb_yielded) {
		uvc_usb_yield_complete = 0;
		ksceKernelClearEventFlag(uvc_event_flag_id, ~UVC_EVENT_USB_YIELDED);
	}
	uvc_usb_yielded = 1;
	uvc_audio_wanted = 0;
	uvc_audio_control_dirty = 1;
	uvc_handle_video_abort();
	ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	if (!uvc_usb_yield_complete) {
		SceUInt32 timeout = 3000000;
		ret = ksceKernelWaitEventFlag(uvc_event_flag_id, UVC_EVENT_USB_YIELDED,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, &timeout);
		if (ret < 0)
			goto out;
	}
	ret = TAI_CONTINUE(int, g_mtp_stop_hook_ref, kind);
	/* Taking UVC over stopped the underlying MTP driver directly, leaving
	 * Sony's service-level started flag intact. Restore just its backend if
	 * necessary so its own Stop can finish normal bookkeeping. No activate
	 * call is made, and another active storage driver is never displaced. */
	if (ret == (int)0x80543601 &&
	    ksceUdcdGetDrvState("USB_MTP_Driver") == VITASTREAM_USB_DRIVER_REGISTERED &&
	    ksceUdcdGetDrvState("USBStor_Driver") != VITASTREAM_USB_DRIVER_STARTED) {
		ret = ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
		if (ret >= 0 || ret == SCE_UDCD_ERROR_INVALID_ARGUMENT) {
			ret = ksceUdcdStart("USB_MTP_Driver", 0, NULL);
			if (ret >= 0)
				ret = TAI_CONTINUE(int, g_mtp_stop_hook_ref, kind);
		}
	}
out:
	diagnostic_record("external MTP stop result", ret);
	ksceKernelUnlockFastMutex(&g_usb_handoff_mutex);
	return ret;
}

static int mtp_start_after_external_owner(int kind)
{
	if (kind != 1 || !uvc_thread_run || !uvc_vitashell_usb_request())
		return TAI_CONTINUE(int, g_mtp_start_hook_ref, kind);
	ksceKernelLockFastMutex(&g_usb_handoff_mutex);
	int ret = TAI_CONTINUE(int, g_mtp_start_hook_ref, kind);
	if (uvc_usb_yielded) {
		/* VitaShell calls this after storage Stop succeeds, including with
		 * the cable unplugged. Retain intent even if MTP cannot activate:
		 * the worker can reclaim an idle bus and retry temporary failures. */
		uvc_usb_resume_pending = 1;
		ksceKernelSetEventFlag(uvc_event_flag_id, 1);
		diagnostic_record("MTP restore completed; USB resume pending", ret);
	}
	ksceKernelUnlockFastMutex(&g_usb_handoff_mutex);
	return ret;
}

static int uvc_install_mtp_handoff(void)
{
	int expected = MTP_HOOK_IDLE;
	if (!atomic_compare_exchange_strong(&g_mtp_stop_hook_uid, &expected, MTP_HOOK_INSTALLING))
		return expected >= 0 ? 0 : SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
	int ret;
	if (!g_usb_handoff_mutex_initialized) {
		ret = ksceKernelInitializeFastMutex(&g_usb_handoff_mutex, "uvc_usb_handoff", 0, NULL);
		if (ret < 0)
			goto failed;
		g_usb_handoff_mutex_initialized = 1;
	}
	/* Install restoration first. If Stop publication needs a retry, the
	 * existing Start hook remains valid and the module remains resident. */
	if (atomic_load(&g_mtp_start_hook_uid) < 0) {
		ret = taiHookFunctionExportForKernel(KERNEL_PID, &g_mtp_start_hook_ref,
			"SceMtpIfDriver", SCE_MTP_IF_FOR_DRIVER_NID,
			SCE_MTP_IF_START_DRIVER_NID, mtp_start_after_external_owner);
		if (ret < 0)
			goto failed;
		atomic_store(&g_mtp_start_hook_uid, ret);
	}
	ret = taiHookFunctionExportForKernel(KERNEL_PID, &g_mtp_stop_hook_ref,
		"SceMtpIfDriver", SCE_MTP_IF_FOR_DRIVER_NID,
		SCE_MTP_IF_STOP_DRIVER_NID, mtp_stop_for_external_owner);
	if (ret < 0)
		goto failed;
	atomic_store(&g_mtp_stop_hook_uid, ret);
	diagnostic_record("install VitaShell MtpIf handoff hooks", 0);
	return 0;
failed:
	atomic_store(&g_mtp_stop_hook_uid, MTP_HOOK_IDLE);
	return ret;
}

static int uvc_usb_prepare_unload(void)
{
	if (atomic_load(&g_mtp_start_hook_uid) >= 0)
		return SCE_KERNEL_STOP_FAIL;
	int expected = MTP_HOOK_IDLE;
	return atomic_compare_exchange_strong(&g_mtp_stop_hook_uid, &expected, MTP_HOOK_CLOSING) ||
		expected == MTP_HOOK_CLOSING ? 0 : SCE_KERNEL_STOP_FAIL;
}

static void uvc_usb_ownership_term(void)
{
	if (g_usb_handoff_mutex_initialized) {
		ksceKernelFinalizeFastMutex(&g_usb_handoff_mutex);
		g_usb_handoff_mutex_initialized = 0;
	}
}

static int uvc_mtp_ready(void)
{
	int device_state = ksceUdcdGetDeviceState();
	return ksceUdcdGetDrvState("USB_MTP_Driver") == VITASTREAM_USB_DRIVER_STARTED &&
	       ksceUdcdGetDrvState("USBStor_Driver") != VITASTREAM_USB_DRIVER_STARTED &&
	       device_state >= 0 && (device_state & VITASTREAM_USB_ACTIVATED);
}

static int uvc_resume_usb(void)
{
	static uint64_t retry_after;
	if (!uvc_thread_run || !uvc_usb_yielded || !uvc_usb_yield_complete ||
	    !uvc_usb_resume_pending || ksceKernelGetSystemTimeWide() < retry_after)
		return 0;
	/* A Stop caller may hold this mutex while waiting for this very worker
	 * to finish handing off. Never block the worker behind that caller. */
	if (ksceKernelTryLockFastMutex(&g_usb_handoff_mutex) < 0)
		return 0;
	int ret = 0;
	if (!uvc_thread_run || !uvc_usb_yielded || !uvc_usb_yield_complete ||
	    !uvc_usb_resume_pending)
		goto out;
	int storage = ksceUdcdGetDrvState("USBStor_Driver");
	if (storage != VITASTREAM_USB_DRIVER_REGISTERED &&
	    storage != VITASTREAM_USB_DRIVER_ABSENT)
		goto out;
	int state = ksceUdcdGetDeviceState();
	if (state < 0 || ((state & VITASTREAM_USB_ACTIVATED) && !uvc_mtp_ready()))
		goto out;
	/* No CONNECTED check: arm the device while unplugged so the next plug
	 * enumerates VitaStream. A completed MTP restore attempt plus an idle
	 * bus is sufficient even if that restore could not activate MTP. */
	ksceKernelClearEventFlag(uvc_event_flag_id, ~UVC_EVENT_USB_YIELDED);
	uvc_usb_yield_complete = 0;
	uvc_usb_yielded = 0;
	ret = uvc_start();
	if (ret < 0) {
		/* Drain again before another attempt; failed activation may have
		 * reached driver Start. Neither attach nor detach clears intent. */
		uvc_usb_yielded = 1;
		retry_after = ksceKernelGetSystemTimeWide() + 1000000;
	} else {
		uvc_usb_resume_pending = 0;
		uvc_recover_pending = 0;
		retry_after = 0;
	}
	diagnostic_record("resume UVC after external USB owner", ret);
out:
	ksceKernelUnlockFastMutex(&g_usb_handoff_mutex);
	return ret;
}

static int uvc_recover_usb(void)
{
	int ret;
	if (!uvc_thread_run || uvc_usb_yielded || !uvc_recover_pending || !uvc_mtp_ready())
		return 0;
	ret = uac_audio_stop_sync();
	if (ret < 0)
		return ret;
	uvc_video_cancel();
	ret = uvc_video_release();
	if (ret < 0)
		return ret;
	/* VitaShell can start another storage session while workers drain. */
	if (ksceKernelTryLockFastMutex(&g_usb_handoff_mutex) < 0)
		return 0;
	ret = 0;
	if (uvc_thread_run && !uvc_usb_yielded && uvc_mtp_ready()) {
		ret = uvc_start();
		if (ret >= 0)
			uvc_recover_pending = 0;
		diagnostic_record("recover UVC after MTP restore", ret);
	}
	ksceKernelUnlockFastMutex(&g_usb_handoff_mutex);
	return ret;
}

static inline unsigned int display_to_iftu_pixelformat(unsigned int fmt)
{
	switch (fmt) {
	case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
		return SCE_IFTU_PIXELFORMAT_BGRX8888;
	case 0x00100000:
		return SCE_IFTU_PIXELFORMAT_RGBX8888;
	case 0x60800000:
		return SCE_IFTU_PIXELFORMAT_BGRA1010102;
	case 0x60900000:
		return SCE_IFTU_PIXELFORMAT_RGBA1010102;
	case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
		return SCE_IFTU_PIXELFORMAT_BGRA5551;
	default:
		return 0;
	}
}

static inline unsigned int display_pixelformat_bpp(unsigned int fmt)
{
	switch (fmt) {
	case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
	case 0x00100000:
	case 0x60800000:
	case 0x60900000:
		return 4;
	case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
		return 2;
	default:
		return 0;
	}
}

static int frame_convert_to_nv12(uintptr_t dst_paddr, const SceDisplayFrameBufInfo *fb_info,
					int dst_width, int dst_height)
{
	uintptr_t src_paddr = fb_info->paddr;
	unsigned int src_width = fb_info->framebuf.width;
	unsigned int src_width_aligned = ALIGN(src_width, 16);
	unsigned int src_pitch = fb_info->framebuf.pitch;
	unsigned int src_height = fb_info->framebuf.height;
	unsigned int src_height_aligned = ALIGN(src_height, 8);
	unsigned int src_pixelfmt = fb_info->framebuf.pixelformat;
	unsigned int src_pixelfmt_bpp = display_pixelformat_bpp(src_pixelfmt);
	uint64_t source_row_size = (uint64_t)src_pitch * src_pixelfmt_bpp;
	uint64_t source_size = (uint64_t)src_pitch * src_height_aligned * src_pixelfmt_bpp;
	void *source_start, *source_end;
	int ret, unlock_ret;

	if (!src_paddr || !fb_info->framebuf.base || fb_info->pid <= 0 ||
	    !src_width || !src_height || src_width > UINT32_MAX - 15u ||
	    src_height > UINT32_MAX - 7u || src_pitch < src_width_aligned ||
	    !src_pixelfmt_bpp || !source_row_size || source_row_size > UINT32_MAX ||
	    src_height_aligned > UINT32_MAX / source_row_size ||
	    !source_size || source_size > UINT32_MAX ||
	    dst_width <= 0 || dst_height <= 0 ||
	    (uintptr_t)fb_info->framebuf.base > UINTPTR_MAX - (source_size - 1) ||
	    src_paddr > UINTPTR_MAX - (source_size - 1) ||
	    ((uint64_t)src_width << 16) / dst_width > UINT32_MAX ||
	    ((uint64_t)src_height << 16) / dst_height > UINT32_MAX)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	/* GetProcFrameBufInternal returns a snapshot without a source lease.
	 * Pin its process and backing memory until IFTU has finished reading it. */
	ret = ksceGUIDReferObject(fb_info->pid, NULL);
	if (ret < 0)
		return ret;
	ret = ksceKernelLockRangeProc(fb_info->pid, fb_info->framebuf.base,
		(SceSize)source_size);
	if (ret < 0)
		goto release_process;
	ret = ksceKernelProcModeVAtoPA(fb_info->pid, 0x10,
		fb_info->framebuf.base, &source_start);
	if (ret >= 0)
		ret = ksceKernelProcModeVAtoPA(fb_info->pid, 0x10,
			(const void *)((uintptr_t)fb_info->framebuf.base + (uintptr_t)source_size - 1),
			&source_end);
	if (ret < 0)
		goto unlock_source;
	/* Drop a stale/replaced or non-contiguous surface rather than reading
	 * its old physical address after an application changes framebuffers. */
	if ((uintptr_t)source_start != src_paddr ||
	    (uintptr_t)source_end != src_paddr + source_size - 1) {
		ret = SCE_UDCD_ERROR_INVALID_ARGUMENT;
		goto unlock_source;
	}

	static SceIftuCscParams RGB_to_YCbCr_JPEG_csc_params = {
		0, 0x202, 0x3FF,
		0, 0x3FF,     0,
		{
			{ 0x99, 0x12C,  0x3A},
			{0xFAA, 0xF57, 0x100},
			{0x100, 0xF2A, 0xFD7}
		}
	};

	SceIftuConvParams params;
	memset(&params, 0, sizeof(params));
	params.size = sizeof(params);
	params.unk04 = 1;
	params.csc_params1 = &RGB_to_YCbCr_JPEG_csc_params;
	params.csc_params2 = NULL;
	params.csc_control = 1;
	params.unk14 = 0;
	params.unk18 = 0;
	params.unk1C = 0;
	params.alpha = 0xFF;
	params.unk24 = 0;

	SceIftuPlaneState_updated src;
	memset(&src, 0, sizeof(src));
	src.fb.pixelformat = display_to_iftu_pixelformat(src_pixelfmt);
	src.fb.width = src_width_aligned;
	src.fb.height = src_height_aligned;
	src.fb.leftover_stride = (src_pitch - src_width_aligned) * src_pixelfmt_bpp;
	src.fb.leftover_align = 0;
	src.fb.paddr0 = src_paddr;
	src.reserved[0] = 0;
	src.reserved[1] = 0;
	src.reserved[2] = 0;
	src.src_w = ((uint64_t)src_width << 16) / dst_width;
	src.src_h = ((uint64_t)src_height << 16) / dst_height;
	src.dst_x = 0;
	src.dst_y = 0;
	src.src_x = 0;
	src.src_y = 0;
	src.crop_top = 0;
	src.crop_bottom = src_height_aligned - src_height;
	src.crop_left = 0;
	src.crop_right = src_width_aligned - src_width;

	SceIftuFrameBuf dst;
	memset(&dst, 0, sizeof(dst));
	dst.pixelformat = SCE_IFTU_PIXELFORMAT_NV12;
	dst.width = dst_width;
	dst.height = dst_height;
	dst.leftover_stride = 0;
	dst.leftover_align = 0;
	dst.paddr0 = dst_paddr;
	dst.paddr1 = dst_paddr + dst_width * dst_height;

	ret = ksceIftuCsc(&dst, (SceIftuPlaneState *)&src, &params);
unlock_source:
	unlock_ret = ksceKernelUnlockRangeProc(fb_info->pid,
		fb_info->framebuf.base, (SceSize)source_size);
	if (ret >= 0 && unlock_ret < 0)
		ret = unlock_ret;
release_process:
	ksceGUIDReleaseObject(fb_info->pid);
	return ret;
}

static int send_frame(void)
{
	static int fid;
	const struct UVC_FRAME_UNCOMPRESSED(2) *frames =
		video_streaming_descriptors.frames_uncompressed_nv12;
	unsigned int frame_index = uvc_probe_control_setting.bFrameIndex;
	unsigned int format_index = uvc_probe_control_setting.bFormatIndex;
	unsigned int frame_count = sizeof(video_streaming_descriptors.frames_uncompressed_nv12) /
		sizeof(video_streaming_descriptors.frames_uncompressed_nv12[0]);
	struct uvc_video_buffer buffer;
	SceDisplayFrameBufInfo fb_info;
	int ret, head, dst_width, dst_height;
#ifdef DEBUG
	uint64_t time1, time2, time3;
#endif

	if (uvc_usb_yielded || format_index != FORMAT_INDEX_UNCOMPRESSED_NV12 ||
		frame_index < 1 || frame_index > frame_count)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	dst_width = frames[frame_index - 1].wWidth;
	dst_height = frames[frame_index - 1].wHeight;
	ret = uvc_video_prepare(VIDEO_FRAME_SIZE_NV12(dst_width, dst_height), &buffer);
	if (ret < 0)
		return ret;
	if (!uvc_thread_run || uvc_usb_yielded || !stream ||
		frame_index != uvc_probe_control_setting.bFrameIndex ||
		format_index != uvc_probe_control_setting.bFormatIndex)
		return 0;

	head = ksceDisplayGetPrimaryHead();
	memset(&fb_info, 0, sizeof(fb_info));
	fb_info.size = sizeof(fb_info);
	ret = ksceDisplayGetProcFrameBufInternal(-1, head, 0, &fb_info);
	if (ret < 0 || fb_info.paddr == 0)
		ret = ksceDisplayGetProcFrameBufInternal(-1, head, 1, &fb_info);
	if (ret < 0)
		return ret;
	if (!fb_info.paddr)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

#ifdef DEBUG
	time1 = ksceKernelGetSystemTimeWide();
#endif
	ret = frame_convert_to_nv12(buffer.data_paddr, &fb_info, dst_width, dst_height);
	if (ret < 0)
		return ret;
#ifdef DEBUG
	time2 = ksceKernelGetSystemTimeWide();
#endif

	ret = uvc_video_wait();
	if (ret < 0)
		return ret;
	if (!uvc_thread_run || uvc_usb_yielded || !stream ||
		frame_index != uvc_probe_control_setting.bFrameIndex ||
		format_index != uvc_probe_control_setting.bFormatIndex)
		return 0;

	ret = uvc_video_submit(fid);
	if (ret < 0)
		return ret;
	fid ^= 1;

	/* A stop can race the enqueue even after the pre-submit check. */
	if (!uvc_thread_run || uvc_usb_yielded || !stream ||
		frame_index != uvc_probe_control_setting.bFrameIndex ||
		format_index != uvc_probe_control_setting.bFormatIndex)
		uvc_video_cancel();
#ifdef DEBUG
	time3 = ksceKernelGetSystemTimeWide();
	LOG("NV12 CSC: %lldus prior xfer wait/queue: %lldus\n", time2 - time1, time3 - time2);
#endif
	return 0;
}

static int display_vblank_cb_func(int notifyId, int notifyCount, int notifyArg, void *common)
{
	static uint64_t phase;
	static uint64_t period;
	static unsigned int last_interval;
	static unsigned int last_epoch;
	unsigned int interval, epoch;

	if (!stream || uvc_usb_yielded) {
		phase = 0;
		return 0;
	}
	if (notifyCount <= 0)
		return 0;

	epoch = atomic_load_explicit(&uvc_pacing_epoch, memory_order_acquire);
	interval = atomic_load_explicit(&uvc_capture_interval, memory_order_relaxed);
	if (!period || interval != last_interval || epoch != last_epoch) {
		phase = 0;
		last_interval = interval;
		last_epoch = epoch;
		period = (uint64_t)interval * 60u;

		/* A 100 ns rounding difference must not cost a whole vblank. */
		uint64_t remainder = period % 10000000u;
		if (remainder <= 60u)
			period -= remainder;
		else if (10000000u - remainder <= 60u)
			period += 10000000u - remainder;
		if (period < 10000000u)
			period = 10000000u;
	}

	phase += (uint64_t)notifyCount * 10000000u;
	if (phase >= period) {
		phase -= period;
		/* Delayed callbacks must not generate a backlog of stale captures. */
		if (phase >= period)
			phase %= period;
		ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	}

	return 0;
}

static int uvc_thread(SceSize args, void *argp)
{
	SceUID display_vblank_cb_uid;
	int start_result;
	int video_progress = 0;
	uint64_t mtp_hook_retry_after = 0;
#if 0
	/*
	 * Wait until the MTP driver starts to takeover.
	 */
	SceUdcdWaitParam wait_param;
	memset(&wait_param, 0, sizeof(wait_param));
	wait_param.status = SCE_UDCD_STATUS_ACTIVATED;
	wait_param.driverName = "USB_MTP_Driver";
	ksceUdcdWaitState(&wait_param, 0);
	ksceKernelDelayThread(250 * 1000);
#endif

	stream = 0;
	diagnostic_record("uvc_thread entered", 0);
	ksceDisplayWaitSetFrameBufCB();
#ifndef DEBUG
	ksceKernelDelayThreadCB(15 * 1000 * 1000);
#endif
	if (!uvc_thread_run)
		return 0;
	vitastream_settings_load();
	/* Storage support is optional at boot. MtpIf can load later; absence
	 * must not leave Sony MTP in control instead of starting capture. */
	start_result = uvc_install_mtp_handoff();
	if (!uvc_thread_run || !g_usb_handoff_mutex_initialized) {
		diagnostic_record("USB handoff initialization failed", start_result);
		return start_result;
	}
	if (start_result < 0)
		diagnostic_record("MtpIf hook deferred; starting capture", start_result);
	mtp_hook_retry_after = ksceKernelGetSystemTimeWide() + 1000000;
	if (ksceKernelTryLockFastMutex(&g_usb_handoff_mutex) < 0) {
		/* Let the MTP request finish. The worker's recovery path will
		 * retry takeover; it must not wait behind a Stop awaiting us. */
		uvc_recover_pending = 1;
		start_result = 0;
	} else {
		if (ksceUdcdGetDrvState("USBStor_Driver") == VITASTREAM_USB_DRIVER_STARTED) {
			/* Never interrupt a storage session opened before us. */
			uvc_usb_yielded = 1;
			uvc_usb_yield_complete = 1;
			start_result = 0;
		} else {
			start_result = uvc_start();
		}
		uvc_recover_pending = 0;
		ksceKernelUnlockFastMutex(&g_usb_handoff_mutex);
	}
	diagnostic_record("uvc_start", start_result);
	if (start_result < 0 && !uvc_usb_yielded)
		return start_result;
	display_vblank_cb_uid = ksceKernelCreateCallback("uvc_display_vblank", 0,
							 display_vblank_cb_func, NULL);

	if (display_vblank_cb_uid < 0) {
		uvc_stop();
		return display_vblank_cb_uid;
	}
	int callback_ret = ksceDisplayRegisterVblankStartCallback(display_vblank_cb_uid);
	if (callback_ret < 0) {
		ksceKernelDeleteCallback(display_vblank_cb_uid);
		uvc_stop();
		return callback_ret;
	}

	while (uvc_thread_run) {
		unsigned int out_bits;

		int ret = ksceKernelWaitEventFlagCB(uvc_event_flag_id, 1,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&out_bits, (SceUInt32[]){1000000});

		if (!uvc_thread_run)
			break;
		if (atomic_load(&g_mtp_stop_hook_uid) < 0 &&
		    ksceKernelGetSystemTimeWide() >= mtp_hook_retry_after) {
			uvc_install_mtp_handoff();
			mtp_hook_retry_after = ksceKernelGetSystemTimeWide() + 1000000;
		}
		if (uvc_usb_yielded) {
			static int last_handoff_error;
			int handoff_ret = uvc_complete_usb_handoff();
			if (handoff_ret < 0 && handoff_ret != last_handoff_error)
				diagnostic_record("USB handoff worker error", handoff_ret);
			last_handoff_error = handoff_ret;
			video_progress = 0;
			vitastream_nosleep_update(0);
			if (handoff_ret >= 0)
				uvc_resume_usb();
			if (uvc_usb_yielded)
				continue;
		}
		uvc_apply_audio_control();
		if (!stream) {
			video_progress = 0;
			uvc_video_cancel();
			uvc_video_release();
		}
		uvc_recover_usb();
		uac_audio_diagnostic_state();
		if (ret == 0 && stream) {
			static int last_error;
			ret = send_frame();
			uvc_video_finish_frame();
			video_progress = ret >= 0;
			if (ret < 0 && ret != last_error) {
				LOG("Error processing NV12 frame: 0x%08X\n", ret);
				diagnostic_record("NV12 frame capture error", ret);
			}
			last_error = ret;
		} else if (ret == (int)0x80028005) {
			video_progress = 0;
			if (!stream)
				uvc_video_cancel();
			uvc_video_release();
		}
		/* A bulk host may stop reading without sending a clean UVC stop.
		 * Release panel blanking after capture stalls instead of leaving
		 * the handheld dark indefinitely on a retained request. */
		{
			static int last_screen_error;
			static int last_nosleep_error;
			int active = (stream && video_progress) || uac_audio_is_active();
			int nosleep_ret = vitastream_nosleep_update(active);
			if (nosleep_ret < 0 && nosleep_ret != last_nosleep_error)
				diagnostic_record("automatic suspend policy error", nosleep_ret);
			last_nosleep_error = nosleep_ret;
			int screen_ret = vitastream_screen_update(active);
			if (screen_ret < 0 && screen_ret != last_screen_error)
				diagnostic_record("local display policy error", screen_ret);
			last_screen_error = screen_ret;
		}
	}

	ksceDisplayUnregisterVblankStartCallback(display_vblank_cb_uid);
	ksceKernelDeleteCallback(display_vblank_cb_uid);

	return uvc_stop();
}

int uvc_start(void)
{
	int ret;
	if (uvc_usb_yielded)
		return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;

	memcpy(&uvc_probe_control_setting, &uvc_probe_control_setting_default,
		sizeof(uvc_probe_control_setting));

	ret = ksceUdcdDeactivate();
	diagnostic_record("ksceUdcdDeactivate", ret);
	if (ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT) {
		LOG("Error deactivating UDCD (0x%08X)\n", ret);
		return ret;
	}

	ksceUdcdStop("USB_MTP_Driver", 0, NULL);
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBPSPCommunicationDriver", 0, NULL);
	ksceUdcdStop("USBSerDriver", 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);

	ret = ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
	diagnostic_record("start USBDeviceControllerDriver", ret);
	if (ret < 0) {
		LOG("Error starting the USBDeviceControllerDriver driver (0x%08X)\n", ret);
		goto restore_mtp;
	}

	ret = ksceUdcdStart(UVC_DRIVER_NAME, 0, NULL);
	diagnostic_record("start " UVC_DRIVER_NAME, ret);
	if (ret < 0) {
		LOG("Error starting the " UVC_DRIVER_NAME " driver (0x%08X)\n", ret);
		goto err_start_uvc_driver;
	}

	if (uvc_usb_yielded) {
		ret = SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
		goto err_activate;
	}
	ret = ksceUdcdActivate(UVC_USB_PID);
	diagnostic_record("activate 0x1337", ret);
	if (ret < 0) {
		LOG("Error activating the " UVC_DRIVER_NAME " driver (0x%08X)\n", ret);
		goto err_activate;
	}

	return 0;

err_activate:
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
err_start_uvc_driver:
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
restore_mtp:
	if (uvc_usb_yielded)
		return ret;
	ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USB_MTP_Driver", 0, NULL);
	ksceUdcdActivate(0x4E4);
	return ret;
}

int uvc_stop(void)
{
	int ret;
	int screen_ret = vitastream_screen_restore();

	ret = uac_audio_stop_sync();
	if (ret < 0)
		LOG("Error stopping UAC audio (0x%08X)\n", ret);

	uvc_video_cancel();
	ret = uvc_video_release();
	if (ret < 0)
		return ret;

	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USB_MTP_Driver", 0, NULL);
	ksceUdcdActivate(0x4E4);

	return screen_ret;
}

static SceUID g_udcd_config_hook_uid = -1;
static tai_hook_ref_t g_udcd_config_hook_ref;

static int uvc_serialize_config_hook(const SceUdcdConfigDescriptor *config_descriptor, void *desc_data)
{
	int ret;
	SceUdcdConfigDescriptor *dst = desc_data;

	ret = TAI_CONTINUE(int, g_udcd_config_hook_ref, config_descriptor, desc_data);

	/*
	 * SceUdcd doesn't use the extra and extraLength members of the
	 * SceUdcdConfigDescriptor struct, so we have to manually patch
	 * it to add custom descriptors.
	 */
	if (ret >= 0 && dst->wTotalLength == config_descriptor->wTotalLength) {
		memmove(desc_data + USB_DT_CONFIG_SIZE + sizeof(interface_association_descriptor),
			desc_data + USB_DT_CONFIG_SIZE,
			config_descriptor->wTotalLength - USB_DT_CONFIG_SIZE);

		memcpy(desc_data + USB_DT_CONFIG_SIZE, interface_association_descriptor,
		       sizeof(interface_association_descriptor));

		dst->wTotalLength += sizeof(interface_association_descriptor);

		ksceKernelDcacheCleanRange(desc_data, dst->wTotalLength);
	}

	return ret;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	int ret;
	SceUID udcd_modid;

	diagnostic_reset();
	diagnostic_record("module_start entered", 0);
	ret = firmware_prepare(&udcd_modid);
	diagnostic_record("firmware compatibility", ret);
	if (ret < 0)
		return SCE_KERNEL_START_NO_RESIDENT;

	g_udcd_config_hook_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
		&g_udcd_config_hook_ref, udcd_modid, 0,
		FIRMWARE_UDCD_CONFIG_OFFSET, 1, uvc_serialize_config_hook);
	diagnostic_record("install USB configuration hook", g_udcd_config_hook_uid);
	if (g_udcd_config_hook_uid < 0)
		return SCE_KERNEL_START_FAILED;

#ifdef DEBUG
	log_reset();
	framebuffer_map();
	console_init();
#endif

	LOG("vitastream (based on vita-udcd-uvc by Xerpi)\n");

	uvc_thread_id = ksceKernelCreateThread("uvc_thread", uvc_thread,
					       0x3C, 0x1000, 0, 0x10000, 0);
	diagnostic_record("create uvc_thread", uvc_thread_id);
	if (uvc_thread_id < 0) {
		LOG("Error creating the UVC thread (0x%08X)\n", uvc_thread_id);
		goto err_return;
	}

	/* The capture worker waits for bit 1; an external USB owner waits for
	 * the handoff acknowledgement on bit 2. Both may wait concurrently. */
	uvc_event_flag_id = ksceKernelCreateEventFlag("uvc_event_flag", SCE_EVENT_WAITMULTIPLE,
						      0, NULL);
	diagnostic_record("create uvc_event_flag", uvc_event_flag_id);
	if (uvc_event_flag_id < 0) {
		LOG("Error creating the UVC event flag (0x%08X)\n", uvc_event_flag_id);
		goto err_destroy_thread;
	}

	ret = uvc_video_init(&endpoints[1]);
	uvc_video_set_wake_event(uvc_event_flag_id);
	diagnostic_record("uvc_video_init", ret);
	if (ret < 0)
		goto err_delete_event_flag;

	ret = uac_audio_init(&endpoints[2]);
	diagnostic_record("uac_audio_init", ret);
	if (ret < 0) {
		LOG("Error initializing UAC audio (0x%08X)\n", ret);
		goto err_video_term;
	}

	ret = ksceUdcdRegister(&uvc_udcd_driver);
	diagnostic_record("ksceUdcdRegister " UVC_DRIVER_NAME, ret);
	if (ret < 0) {
		LOG("Error registering the UDCD driver (0x%08X)\n", ret);
		goto err_audio_term;
	}
	uvc_driver_registered = 1;

	uvc_thread_run = 1;

	ret = ksceKernelStartThread(uvc_thread_id, 0, NULL);
	diagnostic_record("start uvc_thread", ret);
	if (ret < 0) {
		LOG("Error starting the UVC thread (0x%08X)\n", ret);
		goto err_unregister;
	}

	diagnostic_record("module_start success", 0);
	return SCE_KERNEL_START_SUCCESS;

err_unregister:
	if (ksceUdcdUnregister(&uvc_udcd_driver) >= 0)
		uvc_driver_registered = 0;
err_audio_term:
	uac_audio_term();
err_video_term:
	uvc_video_term();
err_delete_event_flag:
	ksceKernelDeleteEventFlag(uvc_event_flag_id);
err_destroy_thread:
	ksceKernelDeleteThread(uvc_thread_id);
err_return:
	taiHookReleaseForKernel(g_udcd_config_hook_uid, g_udcd_config_hook_ref);
	g_udcd_config_hook_uid = -1;
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	if (uvc_usb_prepare_unload() < 0)
		return SCE_KERNEL_STOP_FAIL;
	if (uvc_video_prepare_unload() < 0)
		return SCE_KERNEL_STOP_FAIL;
	if (uac_audio_prepare_unload() < 0)
		return SCE_KERNEL_STOP_FAIL;
	uac_audio_begin_shutdown();
	if (uac_audio_stop_sync() < 0)
		return SCE_KERNEL_STOP_FAIL;

	uvc_thread_run = 0;
	uvc_handle_video_abort();

	ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	if (ksceKernelWaitThreadEnd(uvc_thread_id, NULL, NULL) < 0)
		return SCE_KERNEL_STOP_FAIL;
	if (vitastream_screen_restore() < 0)
		return SCE_KERNEL_STOP_FAIL;
	if (uvc_video_release() < 0)
		return SCE_KERNEL_STOP_FAIL;

	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	if (uvc_driver_registered) {
		if (ksceUdcdUnregister(&uvc_udcd_driver) < 0)
			return SCE_KERNEL_STOP_FAIL;
		uvc_driver_registered = 0;
	}

	if (uvc_video_term() < 0)
		return SCE_KERNEL_STOP_FAIL;
	if (uac_audio_term() < 0)
		return SCE_KERNEL_STOP_FAIL;
	uvc_usb_ownership_term();

	ksceKernelDeleteEventFlag(uvc_event_flag_id);
	ksceKernelDeleteThread(uvc_thread_id);

	if (g_udcd_config_hook_uid >= 0) {
		taiHookReleaseForKernel(g_udcd_config_hook_uid,
			g_udcd_config_hook_ref);
		g_udcd_config_hook_uid = -1;
	}

#ifdef DEBUG
	console_fini();
	framebuffer_unmap();
	log_flush();
#endif

	return SCE_KERNEL_STOP_SUCCESS;
}
