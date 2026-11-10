#ifndef VITASTREAM_VIDEO_DIAGNOSTIC_H
#define VITASTREAM_VIDEO_DIAGNOSTIC_H

#include <stdint.h>

enum video_diag_control {
	VIDEO_DIAG_RESET, VIDEO_DIAG_PROBE, VIDEO_DIAG_COMMIT,
	VIDEO_DIAG_SYSTEM_EVENT, VIDEO_DIAG_SYSTEM_WAIT
};

enum video_diag_stage {
	VIDEO_DIAG_FORMAT, VIDEO_DIAG_PREPARE, VIDEO_DIAG_FRAMEBUFFER,
	VIDEO_DIAG_SCAN_WINDOW, VIDEO_DIAG_GEOMETRY, VIDEO_DIAG_PROCESS_REF,
	VIDEO_DIAG_SOURCE_LOCK, VIDEO_DIAG_VA_START, VIDEO_DIAG_VA_END,
	VIDEO_DIAG_PHYSICAL_SPAN, VIDEO_DIAG_IFTU, VIDEO_DIAG_SOURCE_UNLOCK,
	VIDEO_DIAG_PROCESS_RELEASE, VIDEO_DIAG_PRIOR_USB_WAIT, VIDEO_DIAG_SUBMIT,
	VIDEO_DIAG_USB_EVENT_WAIT, VIDEO_DIAG_USB_COMPLETION,
	VIDEO_DIAG_ALLOCATE, VIDEO_DIAG_BUFFER_BASE, VIDEO_DIAG_BUFFER_PHYSICAL,
	VIDEO_DIAG_FREE, VIDEO_DIAG_STAGE_COUNT
};

#ifdef DIAGNOSTIC
/* Nonblocking, allocation-free producer; safe in the EP0 callback. */
void video_diag_control(enum video_diag_control event, unsigned int format,
	unsigned int frame, unsigned int interval, unsigned int width,
	unsigned int height);
/* Only the video worker consumes records and writes the log. */
void video_diag_flush(void);
void video_diag_system_event(int resume, int event, int state);
void video_diag_system_wait_result(int result);
void video_diag_mode(unsigned int format, unsigned int frame,
	unsigned int interval, unsigned int width, unsigned int height);
void video_diag_source(int head, int pid, uintptr_t base, uintptr_t physical,
	unsigned int width, unsigned int height, unsigned int pitch,
	unsigned int format);
/* Call after releasing any source-page/process leases. Returns result unchanged. */
int video_diag_result(enum video_diag_stage stage, int result);
#else
#define video_diag_control(...) ((void)0)
#define video_diag_flush() ((void)0)
#define video_diag_system_event(...) ((void)0)
#define video_diag_system_wait_result(...) ((void)0)
#define video_diag_mode(...) ((void)0)
#define video_diag_source(...) ((void)0)
static inline int video_diag_result(enum video_diag_stage stage, int result)
{
	(void)stage;
	return result;
}
#endif

#endif
