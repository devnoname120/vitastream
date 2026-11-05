#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2/kernel/error.h>
#include <string.h>
#include "uvc.h"
#include "video.h"
#include "video_camera.h"
#include "settings.h"
#include "diagnostic.h"

#ifndef UVC_FRAMEBUFFER_COUNT
#define UVC_FRAMEBUFFER_COUNT 2
#endif
#if UVC_FRAMEBUFFER_COUNT != 1 && UVC_FRAMEBUFFER_COUNT != 2
#error UVC_FRAMEBUFFER_COUNT must be 1 or 2
#endif
#if defined(UVC_VIDEO_KERNEL_POOL) && !defined(UVC_VIDEO_CAMERA_BORROW) && UVC_FRAMEBUFFER_COUNT != 1
#error The kernel-pool comparison build must use one video buffer
#endif

static struct {
	SceUID uid;
	int borrowed;
	struct uvc_video_buffer buffer;
} slots[UVC_FRAMEBUFFER_COUNT] = {
	{ .uid = -1 },
#if UVC_FRAMEBUFFER_COUNT == 2
	{ .uid = -1 },
#endif
};

static SceUdcdEndpoint *video_endpoint;
static SceUID completion_event = -1;
static SceUdcdDeviceRequest request;
static unsigned int allocated_size;
static unsigned int buffer_count;
static unsigned int next_buffer;
static int pending;
static int prepared;
static unsigned int pending_slot;
#ifdef UVC_VIDEO_CAMERA_BORROW
static uint64_t camera_retry_at;
#endif

void uvc_video_set_wake_event(SceUID event)
{
	video_camera_set_wake_event(event);
}

int uvc_video_prepare_unload(void)
{
	return video_camera_prepare_unload();
}

void uvc_video_finish_frame(void)
{
#ifdef UVC_VIDEO_CAMERA_BORROW
	/* Also runs after conversion errors and all pre-submit early returns. */
	if (!(pending && slots[pending_slot].borrowed))
		video_camera_release();
#endif
	prepared = 0;
}

static void transfer_complete(SceUdcdDeviceRequest *req)
{
	(void)req;
	ksceKernelSetEventFlag(completion_event, 1);
}

int uvc_video_init(SceUdcdEndpoint *endpoint)
{
	if (!endpoint || completion_event >= 0)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	completion_event = ksceKernelCreateEventFlag("uvc_frame_req_evflag", 0, 0, NULL);
	if (completion_event < 0)
		return completion_event;

	video_endpoint = endpoint;
	return 0;
}

int uvc_video_wait(void)
{
	int ret;
	SceUInt32 timeout = 1000000;

	if (!pending)
		return 0;

	ret = ksceKernelWaitEventFlagCB(completion_event, 1,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, &timeout);
	/* A timeout does not return ownership of the request or its DMA buffer. */
	if (ret < 0)
		return ret;

	pending = 0;
#ifdef UVC_VIDEO_CAMERA_BORROW
	if (slots[pending_slot].borrowed)
		video_camera_release();
#endif
	return request.returnCode;
}

static int free_slot(unsigned int slot)
{
	int ret;

	if (slots[slot].borrowed) {
		/* The UID belongs to SceCamera; never pass it to FreeMemBlock. */
		slots[slot].borrowed = 0;
		memset(&slots[slot].buffer, 0, sizeof(slots[slot].buffer));
		return 0;
	}
	if (slots[slot].uid < 0)
		return 0;

	ret = ksceKernelFreeMemBlock(slots[slot].uid);
	if (ret < 0)
		return ret;

	slots[slot].uid = -1;
	memset(&slots[slot].buffer, 0, sizeof(slots[slot].buffer));
	return 0;
}

int uvc_video_release(void)
{
	int ret = uvc_video_wait();
	int error = 0;

	if (pending)
		return ret;
	uvc_video_finish_frame();

	allocated_size = buffer_count = next_buffer = 0;
#ifdef UVC_VIDEO_CAMERA_BORROW
	camera_retry_at = 0;
#endif
	prepared = 0;
	for (unsigned int i = 0; i < UVC_FRAMEBUFFER_COUNT; i++) {
		ret = free_slot(i);
		if (ret < 0 && !error)
			error = ret;
	}
	return error;
}

static int allocate_slot(unsigned int slot, unsigned int frame_size)
{
	SceKernelAllocMemBlockKernelOpt opt;
	unsigned int size = (offsetof(struct uvc_frame, data) + frame_size + 4095u) & ~4095u;
	int ret;

#ifdef UVC_VIDEO_CAMERA_BORROW
	if (slot) {
		if (vitastream_settings_lowmemory_on())
			return -1;
		ret = video_camera_acquire(size, &slots[slot].buffer);
		if (ret < 0) {
			camera_retry_at = ksceKernelGetSystemTimeWide() + 1000000;
			return ret;
		}
		slots[slot].borrowed = 1;
		video_camera_release();
		return 0;
	}
#endif
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4096;
#ifdef UVC_VIDEO_CAMERA_BORROW
	diagnostic_record("owned video allocation requested bytes", size);
#endif
	/* The optional issue #4 comparison uses one bounded kernel buffer so an
	 * application can request the entire shared PhyCont pool. Neither policy
	 * falls back to the other pool on failure. */
	slots[slot].uid = ksceKernelAllocMemBlock("uvc_frame_buffer",
#ifdef UVC_VIDEO_KERNEL_POOL
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW, size, &opt);
#else
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW, size, &opt);
#endif
	if (slots[slot].uid < 0)
		return slots[slot].uid;

	ret = ksceKernelGetMemBlockBase(slots[slot].uid, (void **)&slots[slot].buffer.frame);
	if (ret >= 0)
		ret = ksceKernelGetPaddr(slots[slot].buffer.frame->data,
			&slots[slot].buffer.data_paddr);
	if (ret < 0)
		free_slot(slot);
	return ret;
}

#ifdef UVC_VIDEO_CAMERA_BORROW
static int camera_fallback(void)
{
	int ret = uvc_video_wait();
	if (pending)
		return ret;
	video_camera_release();
	free_slot(1);
	buffer_count = 1;
	next_buffer = 0;
	camera_retry_at = ksceKernelGetSystemTimeWide() + 1000000;
	diagnostic_record("video camera fallback buffers", 1);
	return 0;
}
#endif

int uvc_video_prepare(unsigned int frame_size, struct uvc_video_buffer *buffer)
{
	int ret;

	if (completion_event < 0 || !buffer || !frame_size ||
		frame_size > MAX_UVC_VIDEO_FRAME_SIZE)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	if (!buffer_count || allocated_size != frame_size) {
		ret = uvc_video_release();
		if (ret < 0)
			return ret;

		for (unsigned int i = 0; i < UVC_FRAMEBUFFER_COUNT; i++) {
			ret = allocate_slot(i, frame_size);
			if (ret < 0) {
				if (i == 0 || slots[i].uid >= 0) {
					buffer_count = 0;
					return ret;
				}
				/* Preserve serial capture when a second allocation is unavailable. */
				break;
			}
			buffer_count++;
		}
		allocated_size = frame_size;
		diagnostic_record("video allocated buffers", (int)buffer_count);
	}

#ifdef UVC_VIDEO_CAMERA_BORROW
	int acquired = 0;
	unsigned int size = (offsetof(struct uvc_frame, data) + frame_size + 4095u) & ~4095u;
	if (buffer_count == 2 && video_camera_revoked()) {
		ret = camera_fallback();
		if (ret < 0)
			return ret;
	}
	if (buffer_count == 1 && !vitastream_settings_lowmemory_on() &&
	    ksceKernelGetSystemTimeWide() >= camera_retry_at) {
		camera_retry_at = ksceKernelGetSystemTimeWide() + 1000000;
		if (video_camera_acquire(size, &slots[1].buffer) == 0) {
			slots[1].borrowed = 1;
			buffer_count = 2;
			/* The owned first buffer may still be in USB. Convert into the
			 * newly acquired second buffer while that request completes. */
			next_buffer = 1;
			acquired = 1;
			diagnostic_record("video camera recovered buffers", 2);
		}
	}
	if (buffer_count == 2 && next_buffer == 1 && !acquired &&
	    video_camera_acquire(size, &slots[1].buffer) < 0) {
		ret = camera_fallback();
		if (ret < 0)
			return ret;
	}
#endif
	if (buffer_count == 1) {
		ret = uvc_video_wait();
		if (ret < 0)
			return ret;
	}

	*buffer = slots[next_buffer].buffer;
	prepared = 1;
	return 0;
}

int uvc_video_submit(int fid)
{
	struct uvc_frame *frame;
	int ret;

	if (pending)
		return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
	if (!prepared || completion_event < 0)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;
#ifdef UVC_VIDEO_CAMERA_BORROW
	if (slots[next_buffer].borrowed && video_camera_revoked()) {
		uvc_video_finish_frame();
		return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
	}
#endif

	frame = slots[next_buffer].buffer.frame;
	memset(frame->header, 0, sizeof(frame->header));
	frame->header[0] = UVC_PAYLOAD_HEADER_SIZE;
	frame->header[1] = UVC_STREAM_EOH | UVC_STREAM_EOF | (fid ? UVC_STREAM_FID : 0);
	request = (SceUdcdDeviceRequest){
		.endpoint = video_endpoint,
		.data = frame->header,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = UVC_PAYLOAD_SIZE(allocated_size),
		.onComplete = transfer_complete
	};

	pending = 1;
	pending_slot = next_buffer;
	ret = ksceUdcdReqSend(&request);
	if (ret < 0) {
		pending = 0;
		return ret;
	}
#ifdef UVC_VIDEO_CAMERA_BORROW
	if (slots[next_buffer].borrowed) {
		static int logged;
		if (!logged) {
			diagnostic_record("video camera buffer queued to USB", 1);
			logged = 1;
		}
		/* Close revoke versus enqueue: the pre-submit cancellation may have
		 * run before the request became visible to the USB driver. */
		if (video_camera_revoked())
			uvc_video_cancel();
	}
#endif

	prepared = 0;
	next_buffer = (next_buffer + 1) % buffer_count;
	return 0;
}

void uvc_video_cancel(void)
{
	if (video_endpoint)
		ksceUdcdReqCancelAll(video_endpoint);
}

int uvc_video_term(void)
{
	int ret;

	uvc_video_cancel();
	ret = uvc_video_release();
	if (ret < 0)
		return ret;

	if (completion_event >= 0) {
		ret = ksceKernelDeleteEventFlag(completion_event);
		if (ret < 0)
			return ret;
		completion_event = -1;
	}
	video_endpoint = NULL;
	return 0;
}
