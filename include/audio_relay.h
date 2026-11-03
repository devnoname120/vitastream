#ifndef VITASTREAM_AUDIO_RELAY_H
#define VITASTREAM_AUDIO_RELAY_H

#include <stddef.h>

/* Prepare/write/stop run on the capture worker, never on a USB callback.
 * No DMA callback refers to plugin code. */
int audio_relay_start(void);
int audio_relay_write(const void *pcm, size_t bytes);
/* Failure retains the operation and DMA buffer for a later safe-stop retry. */
int audio_relay_stop(void);
int audio_relay_pending(void);

#endif
