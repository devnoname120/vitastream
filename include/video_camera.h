#ifndef VIDEO_CAMERA_H
#define VIDEO_CAMERA_H

#include "video.h"

#ifdef UVC_VIDEO_CAMERA_BORROW
void video_camera_set_wake_event(SceUID event);
int video_camera_acquire(unsigned int bytes, struct uvc_video_buffer *buffer);
void video_camera_release(void);
int video_camera_revoked(void);
int video_camera_prepare_unload(void);
#else
static inline void video_camera_set_wake_event(SceUID event) { (void)event; }
static inline int video_camera_prepare_unload(void) { return 0; }
#endif

#endif
