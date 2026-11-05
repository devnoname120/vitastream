/* Reuse the USB ownership fixture; exercise production video.c with a camera lease. */
#define main original_video_fixture_main
#include "test_video.c"
#undef main
#include "video_camera.h"

static struct {
	void *base;
	int available, held, revoked, acquisitions, returns, attempts, lowmemory;
	uint64_t now;
} camera;

uint64_t ksceKernelGetSystemTimeWide(void) { return camera.now; }
int vitastream_settings_lowmemory_on(void) { return camera.lowmemory; }
void video_camera_set_wake_event(SceUID event) { (void)event; }
int video_camera_prepare_unload(void) { return camera.acquisitions ? -1 : 0; }
int video_camera_revoked(void) { return camera.revoked; }
int video_camera_acquire(unsigned int bytes, struct uvc_video_buffer *buffer)
{
	assert(!camera.held && bytes <= MAX_UVC_PAYLOAD_TRANSFER_SIZE + 4096);
	camera.attempts++;
	if (!camera.available) return -1;
	camera.revoked = 0;
	camera.held = 1; camera.acquisitions++;
	buffer->frame = camera.base;
	buffer->data_paddr = (uintptr_t)buffer->frame->data;
	return 0;
}
void video_camera_release(void)
{
	if (camera.held) { camera.held = 0; camera.returns++; }
}
static void begin(int available)
{
	memset(&camera, 0, sizeof(camera));
	camera.base = calloc(1, MAX_UVC_PAYLOAD_TRANSFER_SIZE + 4096);
	assert(camera.base); camera.available = available;
	setup();
}
static void end(void)
{
	teardown(); assert(!camera.held); free(camera.base); camera.base = NULL;
}
static void request_camera(void)
{
	camera.revoked = 1;
	camera.available = 0;
	uvc_video_cancel();
}
static struct uvc_video_buffer queue_borrowed(void)
{
	struct uvc_video_buffer b;
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0);
	assert(!camera.held && mock.alloc_calls == 1 && !mock.contiguous_alloc_calls);
	fill(&b, FRAME_SIZE, 1); assert(uvc_video_submit(0) == 0);
	uvc_video_finish_frame();
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && camera.held);
	fill(&b, FRAME_SIZE, 2);
	assert(uvc_video_wait() == 0 && camera.held);
	assert(uvc_video_submit(1) == 0);
	uvc_video_finish_frame(); assert(camera.held);
	return b;
}
int main(void)
{
	struct uvc_video_buffer b, first;
	/* Low-memory mode never touches Camera, including on later frames or a
	 * resolution change, and always retains serial USB ownership. */
	begin(1); camera.lowmemory = 1;
	assert(uvc_video_prepare(FRAME_SIZE, &first) == 0);
	fill(&first, FRAME_SIZE, 20); assert(uvc_video_submit(0) == 0);
	for (int i = 0; i < 3; i++) {
		camera.now += 2000000;
		assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && b.frame == first.frame);
		assert(!mock.queued && !camera.held && !camera.attempts);
		fill(&b, FRAME_SIZE, 21); assert(uvc_video_submit(0) == 0);
	}
	assert(uvc_video_prepare(FRAME_SIZE / 2, &b) == 0);
	assert(!camera.attempts && mock.live_blocks == 1 && mock.alloc_calls == 2);
	end();

	/* The borrowed buffer survives conversion overlap and is returned only
	 * after its own USB request completes, not the preceding owned request. */
	begin(1); b = queue_borrowed();
	assert(uvc_video_prepare(FRAME_SIZE, &first) == 0 && first.frame != b.frame);
	fill(&first, FRAME_SIZE, 3); assert(camera.held);
	assert(uvc_video_wait() == 0 && !camera.held);
	assert(uvc_video_submit(0) == 0); end();

	/* Unavailable camera backing keeps the only allocation in kernel memory. */
	begin(0); assert(uvc_video_prepare(FRAME_SIZE, &b) == 0);
	fill(&b, FRAME_SIZE, 4); assert(uvc_video_submit(0) == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &first) == 0 && first.frame == b.frame);
	assert(mock.alloc_calls == 1 && !mock.contiguous_alloc_calls); end();

	/* Initial unavailability is retried without reallocating the owned first
	 * buffer; promotion preserves overlap with its in-flight USB request. */
	begin(0); assert(uvc_video_prepare(FRAME_SIZE, &first) == 0);
	fill(&first, FRAME_SIZE, 9); assert(uvc_video_submit(0) == 0);
	int attempts = camera.attempts;
	camera.available = 1; camera.now = 999999;
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && b.frame == first.frame);
	assert(camera.attempts == attempts);
	fill(&b, FRAME_SIZE, 10); assert(uvc_video_submit(1) == 0);
	camera.now = 1000000;
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && b.frame != first.frame);
	assert(camera.held && mock.queued && mock.alloc_calls == 1);
	fill(&b, FRAME_SIZE, 11); assert(uvc_video_wait() == 0 && camera.held);
	assert(uvc_video_submit(0) == 0); end();

	/* Handoff while USB owns camera memory drains it before serial fallback. */
	begin(1); queue_borrowed(); request_camera(); assert(camera.held);
	uvc_video_finish_frame(); assert(camera.held);
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && !camera.held);
	assert(mock.alloc_calls == 1); end();

	/* Camera use can repeat. Keep serial capture while busy, then automatically
	 * regain the second buffer after each close, with no plugin/USB restart. */
	begin(1); queue_borrowed();
	for (int i = 0; i < 3; i++) {
		request_camera();
		assert(uvc_video_prepare(FRAME_SIZE, &first) == 0 && !camera.held);
		fill(&first, FRAME_SIZE, 12); assert(uvc_video_submit(0) == 0);
		camera.now += 1000000;
		assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && b.frame == first.frame);
		fill(&b, FRAME_SIZE, 13); assert(uvc_video_submit(1) == 0);
		camera.available = 1; camera.now += 1000000;
		assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && b.frame != first.frame);
		assert(camera.held && mock.alloc_calls == 1);
		fill(&b, FRAME_SIZE, 14); assert(uvc_video_wait() == 0 && camera.held);
		assert(uvc_video_submit(0) == 0);
	}
	end();

	/* Host stall: timeout is not permission to return or free camera memory. */
	begin(1); queue_borrowed(); mock.auto_complete = mock.cancel_complete = 0;
	request_camera(); assert(uvc_video_prepare(FRAME_SIZE, &b) == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	assert(camera.held && mock.live_blocks == 1);
	assert(uvc_video_release() == SCE_KERNEL_ERROR_WAIT_TIMEOUT && camera.held);
	complete(-3); assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && !camera.held); end();

	/* Conversion failure before submission returns the lease even when the
	 * preceding request still uses the separately-owned first buffer. */
	begin(1); assert(uvc_video_prepare(FRAME_SIZE, &b) == 0);
	fill(&b, FRAME_SIZE, 5); assert(uvc_video_submit(0) == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0 && camera.held);
	uvc_video_finish_frame(); assert(!camera.held && mock.queued); end();

	/* Revoke after conversion but before enqueue must not submit camera RAM. */
	begin(1); assert(uvc_video_prepare(FRAME_SIZE, &b) == 0);
	fill(&b, FRAME_SIZE, 6); assert(uvc_video_submit(0) == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0); request_camera();
	(void)uvc_video_wait();
	assert(uvc_video_submit(1) == SCE_UDCD_ERROR_DRIVER_IN_PROGRESS && !camera.held);
	end();

	/* Revoke racing enqueue requires a cancellation after enqueue as well. */
	begin(1); assert(uvc_video_prepare(FRAME_SIZE, &b) == 0);
	fill(&b, FRAME_SIZE, 7); assert(uvc_video_submit(0) == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &b) == 0); assert(uvc_video_wait() == 0);
	mock.on_send = request_camera; assert(uvc_video_submit(1) == 0);
	assert(mock.cancels >= 2 && camera.held);
	assert(uvc_video_wait() == -3 && !camera.held); end();

	/* Failed USB enqueue and resolution changes cannot leak a native mutex. */
	begin(1); queue_borrowed(); assert(uvc_video_wait() == 0);
	assert(uvc_video_prepare(FRAME_SIZE / 2, &b) == 0);
	fill(&b, FRAME_SIZE / 2, 8); assert(uvc_video_submit(0) == 0);
	assert(uvc_video_prepare(FRAME_SIZE / 2, &b) == 0 && camera.held);
	assert(uvc_video_wait() == 0); mock.fail_send = 1;
	assert(uvc_video_submit(1) == MOCK_ERROR); uvc_video_finish_frame();
	assert(!camera.held && uvc_video_prepare_unload() < 0); end();
	puts("video camera: ownership, repeated handoff, automatic recovery and cancellation passed");
	return 0;
}
