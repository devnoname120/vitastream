#ifndef VITASTREAM_AUDIO_ROUTE_H
#define VITASTREAM_AUDIO_ROUTE_H

#include <stdint.h>

typedef int (*AudioRouteSetModeFn)(int path, int mode);
typedef int (*AudioRouteReadFn)(int path, void *pcm, int frames);

int audio_route_guard_init(void);
void audio_route_guard_term(void);
int audio_route_guard_install(void);
void audio_route_guard_set_owned(int owned);
int audio_route_guard_capture_mode(int path, uintptr_t native_data,
	AudioRouteSetModeFn set_mode);
int audio_route_guard_restore_mode(int path, AudioRouteSetModeFn set_mode);
/* A positive result means ownership changed: do not touch that reader. */
int audio_route_guard_read_primary(AudioRouteReadFn read, void *pcm, int frames);
int audio_route_guard_path_owned(int path);
int audio_route_guard_path_taken(int path);
int audio_route_guard_prepare_unload(void);
void audio_route_guard_diagnostic(void);

#endif
