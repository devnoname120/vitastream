#ifndef VITASTREAM_SETTINGS_H
#define VITASTREAM_SETTINGS_H

/* Called by the video worker before USB activation, never by a USB callback. */
void vitastream_settings_load(void);
int vitastream_settings_speakers_on(void);
int vitastream_settings_lowmemory_on(void);
/* Worker-only panel policy. active means video and/or USB audio is in use. */
int vitastream_screen_update(int active);
int vitastream_screen_restore(void);

#endif
