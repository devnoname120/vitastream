#ifndef VITASTREAM_CONFIG_H
#define VITASTREAM_CONFIG_H

#include <stddef.h>

#define VITASTREAM_CONFIG_MAX_SIZE 1024u
#define VITASTREAM_CONFIG_PATH "ur0:/tai/vitastream.cfg"

struct vitastream_config {
	int screen_on;
	int speakers_on;
	int lowmemory_on;
	int nosleep_on;
};

/* Default to local playback/screen/nosleep on, with low-memory mode off. */
void vitastream_config_defaults(struct vitastream_config *config);
/* Parse atomically: an invalid file never partially changes the destination. */
int vitastream_config_parse(const char *text, size_t length,
	struct vitastream_config *config);

#endif
